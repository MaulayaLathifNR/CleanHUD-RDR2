// =============================================================================
//  Clean HUD  -  Red Dead Redemption 2 ASI mod (ScriptHookRDR2)
//  One hotkey hides the whole HUD while the aiming reticle stays visible.
//  Controller support: button combo (default LB + D-Pad Down) read through XInput,
//  configurable in the [Controller] section of CleanHUD.ini.
//  XInput is linked normally (xinput.lib, see the #pragma below): no LoadLibrary /
//  GetProcAddress anywhere in this file.
//
//  Build: see BUILD.md.  Config: CleanHUD.ini.  Log: CleanHUD.log.
//
//  NATIVES USED  (checked against the natives.h of your ScriptHookRDR2 SDK,
//  the RDR2Mods NativeDB, the RedM native docs and femga/rdr3_discoveries)
//  -----------------------------------------------------------------------------
//   UI::DISPLAY_HUD(BOOL)              0xD63FE3AF9FB3D53F  in SDK + NativeDB
//   UI::IS_HUD_HIDDEN()                0x71B72B478F8189DC  in SDK + NativeDB
//   RADAR::DISPLAY_RADAR(BOOL)         0x1B3DA717B9AFF828  in SDK
//   UI::IS_RADAR_HIDDEN()              0x1B82FD5FFA4D666E  in SDK
//   UI::CLEAR_ALL_HELP_MESSAGES()      0x916ED8321F087059  in SDK
//   _ENABLE_HUD_CONTEXT_THIS_FRAME     0xC9CAEAEEC1256E54  hash in SDK (unnamed)
//        (older NativeDB name: _ENABLE_HUD_COMPONENT_THIS_FRAME)
//
//  HUD CONTEXTS: RDR2 has no "hide component N" native like GTA V. It uses named
//  "HUD contexts" (joaat hashes such as HUD_CTX_INFINITE_AMMO). The hash values
//  below are re-derived at compile time with Joaat() and static_assert-ed
//  against the values documented by femga/rdr3_discoveries.
//
//  WHAT IS *NOT* VERIFIED (no game was available to run this):
//   * That DISPLAY_HUD(false) leaves the reticle visible with every weapon.
//     Confirmed in practice: it does NOT, so Full mode draws its own dot (DotMode).
//   * HUD_CTX_INFINITE_AMMO hiding the ammo counter is a community report.
//   * HUD_CTX_HACK_RADAR_FORCE_HIDE is judged from its name only.
//  The context list is deliberately explicit, and never contains a context
//  known to remove the reticle (see kForbiddenContexts).
// =============================================================================

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX            // windows.h min/max macros break std::min/std::max

#include <windows.h>
#include <xinput.h>
#pragma comment(lib, "xinput.lib")   // normal import-library link, nothing to configure

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "main.h"
#include "natives.h"

// -----------------------------------------------------------------------------
//  Constants
// -----------------------------------------------------------------------------

// Compile-time joaat (Jenkins one-at-a-time, lower-cased) = RDR2 string hash.
constexpr uint32_t Joaat(const char* s)
{
    uint32_t h = 0;
    for (; *s; ++s)
    {
        char c = *s;
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
        h += static_cast<uint8_t>(c);
        h += (h << 10);
        h ^= (h >> 6);
    }
    h += (h << 3);
    h ^= (h >> 11);
    h += (h << 15);
    return h;
}

// Contexts we use, with the hash documented in femga/rdr3_discoveries.
constexpr uint32_t CTX_INFINITE_AMMO      = 0x3F129E06;  // hides ammo counter
constexpr uint32_t CTX_RADAR_FORCE_HIDE   = 0x1C43984E;  // hides radar
constexpr uint32_t CTX_FAST_TRAVEL_MENU   = 0xBB47198C;  // REMOVES RETICLE
constexpr uint32_t CTX_SHARP_SHOOTER      = 0xFC0F918A;  // used for reticle hiding

static_assert(Joaat("HUD_CTX_INFINITE_AMMO")           == CTX_INFINITE_AMMO,    "hash mismatch");
static_assert(Joaat("HUD_CTX_HACK_RADAR_FORCE_HIDE")   == CTX_RADAR_FORCE_HIDE, "hash mismatch");
static_assert(Joaat("HUD_CTX_IN_FAST_TRAVEL_MENU")     == CTX_FAST_TRAVEL_MENU, "hash mismatch");
static_assert(Joaat("HUD_CTX_SHARP_SHOOTER_EVENT")     == CTX_SHARP_SHOOTER,    "hash mismatch");

// Contexts that must never be applied, because they take the reticle away.
static const uint32_t kForbiddenContexts[] = { CTX_FAST_TRAVEL_MENU, CTX_SHARP_SHOOTER };

// Game controls (hold-to-open UIs), from femga/rdr3_discoveries "Controls".
constexpr uint32_t CTRL_OPEN_WHEEL_MENU         = 0xAC4BD4F1;  // Tab (hold) = weapon wheel
constexpr uint32_t CTRL_OPEN_SATCHEL_MENU       = 0x4CC0E2FE;  // B
constexpr uint32_t CTRL_OPEN_SATCHEL_HORSE_MENU = 0x5966D52A;  // B (on horse)
constexpr ULONGLONG kPeekLingerMs = 250;   // keep HUD up briefly after the button is released

constexpr UINT64 NATIVE_ENABLE_HUD_CONTEXT_THIS_FRAME = 0xC9CAEAEEC1256E54ull;

constexpr ULONGLONG kResumeDelayMs = 500;   // wait after a cutscene/fade before re-hiding
constexpr ULONGLONG kToastMs       = 2000;  // "HUD Hidden" message duration
constexpr ULONGLONG kToastFadeMs   = 400;
constexpr BYTE      kPadTriggerThreshold = 100;  // LT/RT count as "pressed" above this (0-255)

// -----------------------------------------------------------------------------
//  Logging  (CleanHUD.log, next to the .asi)
// -----------------------------------------------------------------------------

static HMODULE     g_module = nullptr;
static std::string g_dir;        // directory of the .asi, with trailing backslash
static std::string g_logPath;

static void LogWrite(const char* level, const char* fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    char stamp[32] = "";
    time_t t = time(nullptr);
    strftime(stamp, sizeof(stamp), "%H:%M:%S", localtime(&t));

    if (FILE* f = fopen(g_logPath.c_str(), "a"))
    {
        fprintf(f, "[%s] [%s] %s\n", stamp, level, msg);
        fclose(f);
    }
}
#define LOG_INFO(...) LogWrite("INFO",  __VA_ARGS__)
#define LOG_WARN(...) LogWrite("WARN",  __VA_ARGS__)

// -----------------------------------------------------------------------------
//  Small string helpers
// -----------------------------------------------------------------------------

static std::string Trim(const std::string& s)
{
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

static std::string Upper(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

static std::string Lower(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// -----------------------------------------------------------------------------
//  Key-name -> virtual-key lookup
//    A-Z  = 0x41-0x5A      F1-F12 = VK_F1-VK_F12      0-9 (top row) = 0x30-0x39
// -----------------------------------------------------------------------------

enum class KeyParse { Ok, Disabled, Empty, Invalid };

static KeyParse KeyNameToVK(const std::string& raw, int& vk)
{
    vk = 0;
    const std::string name = Upper(Trim(raw));

    if (name.empty())
        return KeyParse::Empty;

    if (name == "NONE" || name == "OFF" || name == "DISABLED")
        return KeyParse::Disabled;

    // Single character: letter or top-row digit.
    if (name.size() == 1)
    {
        const char c = name[0];
        if (c >= 'A' && c <= 'Z') { vk = c;  return KeyParse::Ok; }   // 0x41-0x5A
        if (c >= '0' && c <= '9') { vk = c;  return KeyParse::Ok; }   // 0x30-0x39
        return KeyParse::Invalid;
    }

    // Function keys: "F" followed by 1-12 (no leading zero, no extra text).
    if (name[0] == 'F' && name.size() <= 3 && name[1] != '0')
    {
        bool digits = true;
        for (size_t i = 1; i < name.size(); ++i)
            if (!std::isdigit(static_cast<unsigned char>(name[i]))) digits = false;

        if (digits)
        {
            const int n = std::atoi(name.c_str() + 1);
            if (n >= 1 && n <= 12) { vk = VK_F1 + (n - 1); return KeyParse::Ok; }
        }
    }
    return KeyParse::Invalid;
}

static std::string VKToName(int vk)
{
    if (vk == 0) return "None";
    if (vk >= VK_F1 && vk <= VK_F12) return "F" + std::to_string(vk - VK_F1 + 1);
    return std::string(1, static_cast<char>(vk));  // letters / digits
}

// -----------------------------------------------------------------------------
//  INI parser  ([section] / key=value, ';' and '#' comments, case-insensitive)
// -----------------------------------------------------------------------------

class IniFile
{
public:
    bool Load(const std::string& path)
    {
        m_.clear();
        std::ifstream in(path);
        if (!in.is_open()) return false;

        std::string line, section;
        bool first = true;
        while (std::getline(in, line))
        {
            if (first && line.size() >= 3 &&
                static_cast<unsigned char>(line[0]) == 0xEF &&
                static_cast<unsigned char>(line[1]) == 0xBB &&
                static_cast<unsigned char>(line[2]) == 0xBF)
                line.erase(0, 3);                      // strip UTF-8 BOM
            first = false;

            line = Trim(line);
            if (line.empty() || line[0] == ';' || line[0] == '#') continue;

            if (line[0] == '[')
            {
                const size_t close = line.find(']');
                if (close != std::string::npos)
                    section = Lower(Trim(line.substr(1, close - 1)));
                continue;
            }

            const size_t eq = line.find('=');
            if (eq == std::string::npos) continue;

            std::string key   = Lower(Trim(line.substr(0, eq)));
            std::string value = line.substr(eq + 1);
            const size_t cmt  = value.find_first_of(";#");   // inline comment
            if (cmt != std::string::npos) value.erase(cmt);

            m_[section + "." + key] = Trim(value);
        }
        return true;
    }

    bool Has(const char* sec, const char* key) const
    {
        return m_.count(Lower(sec) + "." + Lower(key)) != 0;
    }

    std::string Get(const char* sec, const char* key, const std::string& def = "") const
    {
        auto it = m_.find(Lower(sec) + "." + Lower(key));
        return it == m_.end() ? def : it->second;
    }

    bool GetBool(const char* sec, const char* key, bool def) const
    {
        if (!Has(sec, key)) return def;
        const std::string v = Lower(Get(sec, key));
        if (v == "true" || v == "1" || v == "yes" || v == "on")  return true;
        if (v == "false" || v == "0" || v == "no" || v == "off") return false;
        LOG_WARN("%s: '%s' is not a valid true/false value, using default (%s).",
                 key, v.c_str(), def ? "true" : "false");
        return def;
    }

    int GetInt(const char* sec, const char* key, int def, int lo, int hi) const
    {
        if (!Has(sec, key)) return def;
        const std::string v = Get(sec, key);
        char* end = nullptr;
        const long n = std::strtol(v.c_str(), &end, 10);
        if (v.empty() || (end && *end != '\0'))
        {
            LOG_WARN("%s: '%s' is not a number, using default (%d).", key, v.c_str(), def);
            return def;
        }
        const int clamped = static_cast<int>((std::max<long>)(lo, (std::min<long>)(hi, n)));
        if (clamped != n)
            LOG_WARN("%s: %ld is out of range %d-%d, clamped to %d.", key, n, lo, hi, clamped);
        return clamped;
    }

private:
    std::unordered_map<std::string, std::string> m_;
};

// -----------------------------------------------------------------------------
//  Configuration
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
//  Controller combos
//  Read through XInput (Xbox pads, or any pad exposed as XInput via Steam Input /
//  DS4Windows).
// -----------------------------------------------------------------------------

constexpr WORD PAD_DPAD_UP    = XINPUT_GAMEPAD_DPAD_UP;
constexpr WORD PAD_DPAD_DOWN  = XINPUT_GAMEPAD_DPAD_DOWN;
constexpr WORD PAD_DPAD_LEFT  = XINPUT_GAMEPAD_DPAD_LEFT;
constexpr WORD PAD_DPAD_RIGHT = XINPUT_GAMEPAD_DPAD_RIGHT;
constexpr WORD PAD_START      = XINPUT_GAMEPAD_START;
constexpr WORD PAD_BACK       = XINPUT_GAMEPAD_BACK;
constexpr WORD PAD_LS         = XINPUT_GAMEPAD_LEFT_THUMB;    // left stick click (L3)
constexpr WORD PAD_RS         = XINPUT_GAMEPAD_RIGHT_THUMB;   // right stick click (R3)
constexpr WORD PAD_LB         = XINPUT_GAMEPAD_LEFT_SHOULDER;
constexpr WORD PAD_RB         = XINPUT_GAMEPAD_RIGHT_SHOULDER;
constexpr WORD PAD_A          = XINPUT_GAMEPAD_A;
constexpr WORD PAD_B          = XINPUT_GAMEPAD_B;
constexpr WORD PAD_X          = XINPUT_GAMEPAD_X;
constexpr WORD PAD_Y          = XINPUT_GAMEPAD_Y;

// A combo = every listed button/trigger held at the same time.
struct PadCombo
{
    WORD buttons = 0;
    bool lt = false;
    bool rt = false;

    bool Valid() const { return buttons != 0 || lt || rt; }
    int  Count() const
    {
        int n = (lt ? 1 : 0) + (rt ? 1 : 0);
        for (WORD b = buttons; b; b = static_cast<WORD>(b & (b - 1))) ++n;
        return n;
    }
    bool operator==(const PadCombo& o) const
    { return buttons == o.buttons && lt == o.lt && rt == o.rt; }
};

enum class PadParse { Ok, Disabled, Empty, Invalid };

struct PadName { const char* name; WORD bit; int trigger; };   // trigger: 1 = LT, 2 = RT
static const PadName kPadNames[] = {
    { "A", PAD_A, 0 }, { "B", PAD_B, 0 }, { "X", PAD_X, 0 }, { "Y", PAD_Y, 0 },
    { "LB", PAD_LB, 0 }, { "L1", PAD_LB, 0 }, { "LEFTBUMPER", PAD_LB, 0 }, { "LEFTSHOULDER", PAD_LB, 0 },
    { "RB", PAD_RB, 0 }, { "R1", PAD_RB, 0 }, { "RIGHTBUMPER", PAD_RB, 0 }, { "RIGHTSHOULDER", PAD_RB, 0 },
    { "LT", 0, 1 }, { "L2", 0, 1 }, { "LEFTTRIGGER", 0, 1 },
    { "RT", 0, 2 }, { "R2", 0, 2 }, { "RIGHTTRIGGER", 0, 2 },
    { "LS", PAD_LS, 0 }, { "L3", PAD_LS, 0 }, { "LEFTSTICK", PAD_LS, 0 },
    { "RS", PAD_RS, 0 }, { "R3", PAD_RS, 0 }, { "RIGHTSTICK", PAD_RS, 0 },
    { "BACK", PAD_BACK, 0 }, { "SELECT", PAD_BACK, 0 }, { "VIEW", PAD_BACK, 0 },
    { "START", PAD_START, 0 }, { "MENU", PAD_START, 0 },
    { "DPADUP", PAD_DPAD_UP, 0 },       { "DUP", PAD_DPAD_UP, 0 },    { "UP", PAD_DPAD_UP, 0 },
    { "DPADDOWN", PAD_DPAD_DOWN, 0 },   { "DDOWN", PAD_DPAD_DOWN, 0 }, { "DOWN", PAD_DPAD_DOWN, 0 },
    { "DPADLEFT", PAD_DPAD_LEFT, 0 },   { "DLEFT", PAD_DPAD_LEFT, 0 }, { "LEFT", PAD_DPAD_LEFT, 0 },
    { "DPADRIGHT", PAD_DPAD_RIGHT, 0 }, { "DRIGHT", PAD_DPAD_RIGHT, 0 }, { "RIGHT", PAD_DPAD_RIGHT, 0 },
};

// "LB+DPadDown", "lb + dpad_down", "LT+RT+A" ...   None / Off / Disabled = disabled.
static PadParse ParsePadCombo(const std::string& raw, PadCombo& out)
{
    out = PadCombo{};
    const std::string whole = Trim(raw);
    if (whole.empty()) return PadParse::Empty;

    const std::string up = Upper(whole);
    if (up == "NONE" || up == "OFF" || up == "DISABLED") return PadParse::Disabled;

    size_t pos = 0;
    while (true)
    {
        size_t plus = whole.find('+', pos);
        if (plus == std::string::npos) plus = whole.size();

        std::string tok;                                   // normalise: no spaces, '_' or '-'
        for (char ch : whole.substr(pos, plus - pos))
            if (ch != ' ' && ch != '\t' && ch != '_' && ch != '-')
                tok += static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        if (tok.empty()) { out = PadCombo{}; return PadParse::Invalid; }

        bool found = false;
        for (const PadName& n : kPadNames)
        {
            if (tok != n.name) continue;
            out.buttons = static_cast<WORD>(out.buttons | n.bit);
            if (n.trigger == 1) out.lt = true;
            if (n.trigger == 2) out.rt = true;
            found = true;
            break;
        }
        if (!found) { out = PadCombo{}; return PadParse::Invalid; }

        if (plus >= whole.size()) break;
        pos = plus + 1;
    }
    return PadParse::Ok;
}

static std::string PadComboName(const PadCombo& c)
{
    if (!c.Valid()) return "None";
    static const struct { WORD bit; const char* n; } kBits[] = {
        { PAD_LB, "LB" }, { PAD_RB, "RB" }, { PAD_A, "A" }, { PAD_B, "B" }, { PAD_X, "X" }, { PAD_Y, "Y" },
        { PAD_LS, "LS" }, { PAD_RS, "RS" }, { PAD_BACK, "Back" }, { PAD_START, "Start" },
        { PAD_DPAD_UP, "DPadUp" }, { PAD_DPAD_DOWN, "DPadDown" },
        { PAD_DPAD_LEFT, "DPadLeft" }, { PAD_DPAD_RIGHT, "DPadRight" },
    };
    std::string s;
    auto add = [&](const char* n) { if (!s.empty()) s += "+"; s += n; };
    if (c.lt) add("LT");
    if (c.rt) add("RT");
    for (const auto& b : kBits) if (c.buttons & b.bit) add(b.n);
    return s;
}

enum class HideMode { Full, Soft };
enum class DotMode  { Off, Aiming, Always };

struct Config
{
    bool     autoShowWhenAiming = false;
    bool     showOnLowHealth    = false;
    int      lowHealthPercent   = 25;
    int      lowHealthSeconds   = 4;
    bool     toggleMessage      = true;
    bool     resetOnDeath       = true;
    HideMode mode               = HideMode::Full;
    DotMode  dotMode            = DotMode::Aiming;  // own center dot while HUD hidden
    int      dotSize            = 3;                // diameter in pixels
    int      dotAlpha           = 170;              // 0-255
    bool     peekTabKey         = false;            // WeaponWheel listed -> also watch the Tab key
    std::vector<uint32_t> peekControls;                 // game buttons that reveal the HUD while held
    bool     clearHelpText      = true;
    std::vector<uint32_t> extraContexts;
    bool     padEnabled         = true;             // controller shortcuts on/off
    int      padIndex           = -1;               // -1 = any connected pad, 0-3 = that slot only
    PadCombo padToggle;                             // filled in LoadConfig (default LB+DPadDown)
    PadCombo padMinimap;
    PadCombo padReload;
};

static Config g_cfg;   // only touched from the script thread

// The key codes are read by the keyboard callback (another thread) -> atomics.
static std::atomic<int> g_vkToggle{ VK_F11 };
static std::atomic<int> g_vkMinimap{ 0 };
static std::atomic<int> g_vkReload{ 0 };

// Resolve one key setting. `isToggle` = mandatory key with F11 fallback.
static int ResolveKey(const IniFile& ini, const char* name, bool isToggle)
{
    const bool present = ini.Has("Keys", name);
    int vk = 0;
    const KeyParse r = KeyNameToVK(ini.Get("Keys", name), vk);

    if (isToggle)
    {
        if (r == KeyParse::Ok) return vk;
        LOG_WARN("%s is %s ('%s'); falling back to F11.", name,
                 !present ? "missing" : (r == KeyParse::Empty ? "empty" :
                 (r == KeyParse::Disabled ? "'None' (not allowed for the toggle)" : "invalid")),
                 ini.Get("Keys", name).c_str());
        return VK_F11;
    }

    // Optional keys (MinimapKey / ReloadKey): default None.
    if (!present || r == KeyParse::Disabled) return 0;
    if (r == KeyParse::Ok) return vk;
    LOG_WARN("%s is %s ('%s'); falling back to None (disabled).", name,
             r == KeyParse::Empty ? "empty" : "invalid", ini.Get("Keys", name).c_str());
    return 0;
}

// Resolve one controller combo. defText = fallback for missing/invalid values
// (nullptr = fall back to disabled). "None" always disables.
static PadCombo ResolvePadCombo(const IniFile& ini, const char* name, const char* defText)
{
    PadCombo c;
    const bool present = ini.Has("Controller", name);
    const std::string text = ini.Get("Controller", name);
    const PadParse r = present ? ParsePadCombo(text, c) : PadParse::Empty;

    if (r == PadParse::Ok)       return c;
    if (r == PadParse::Disabled) return PadCombo{};

    LOG_WARN("%s is %s ('%s'); falling back to %s.", name,
             !present ? "missing" : (r == PadParse::Empty ? "empty" : "invalid"),
             text.c_str(), defText ? defText : "None (disabled)");
    c = PadCombo{};
    if (defText) ParsePadCombo(defText, c);
    return c;
}

static bool IsForbiddenContext(uint32_t h)
{
    for (uint32_t f : kForbiddenContexts) if (f == h) return true;
    return false;
}

static void ParseExtraContexts(const std::string& list, std::vector<uint32_t>& out)
{
    size_t pos = 0;
    while (pos <= list.size())
    {
        size_t comma = list.find(',', pos);
        if (comma == std::string::npos) comma = list.size();
        const std::string token = Trim(list.substr(pos, comma - pos));
        pos = comma + 1;
        if (token.empty()) continue;

        uint32_t h;
        if (token.size() > 2 && token[0] == '0' && (token[1] == 'x' || token[1] == 'X'))
            h = static_cast<uint32_t>(std::strtoul(token.c_str(), nullptr, 16));
        else
            h = Joaat(token.c_str());

        if (IsForbiddenContext(h))
            LOG_WARN("ExtraContexts: '%s' removes the aiming reticle and was ignored.", token.c_str());
        else
        {
            out.push_back(h);
            LOG_INFO("ExtraContexts: '%s' -> 0x%08X", token.c_str(), h);
        }
        if (comma >= list.size()) break;
    }
}

// PeekWhileHeld=WeaponWheel, Satchel, 0xHASH ...   (None / empty = disabled)
static void ParsePeekControls(const std::string& list, std::vector<uint32_t>& out)
{
    size_t pos = 0;
    while (pos <= list.size())
    {
        size_t comma = list.find(',', pos);
        if (comma == std::string::npos) comma = list.size();
        const std::string token = Upper(Trim(list.substr(pos, comma - pos)));
        pos = comma + 1;

        if (!token.empty() && token != "NONE")
        {
            uint32_t h = 0;
            if (token == "WEAPONWHEEL")        h = CTRL_OPEN_WHEEL_MENU;
            else if (token == "SATCHEL")       h = CTRL_OPEN_SATCHEL_MENU;
            else if (token == "HORSESATCHEL")  h = CTRL_OPEN_SATCHEL_HORSE_MENU;
            else if (token.size() > 2 && token[0] == '0' && token[1] == 'X')
                h = static_cast<uint32_t>(std::strtoul(token.c_str(), nullptr, 16));

            if (h) { out.push_back(h); LOG_INFO("PeekWhileHeld: '%s' -> 0x%08X", token.c_str(), h); }
            else   LOG_WARN("PeekWhileHeld: '%s' is not valid (use WeaponWheel, Satchel, HorseSatchel or 0xHASH); ignored.", token.c_str());
        }
        if (comma >= list.size()) break;
    }
}

static void LoadConfig(bool isReload)
{
    LOG_INFO(isReload ? "Reloading CleanHUD.ini ..." : "Reading CleanHUD.ini ...");

    IniFile ini;
    const std::string path = g_dir + "CleanHUD.ini";
    if (!ini.Load(path))
        LOG_WARN("Could not open %s - using built-in defaults.", path.c_str());

    Config c;
    int toggle  = ResolveKey(ini, "ToggleKey",  true);
    int minimap = ResolveKey(ini, "MinimapKey", false);
    int reload  = ResolveKey(ini, "ReloadKey",  false);

    // Two actions on one key would fire together: keep the first, drop the later.
    if (minimap && minimap == toggle)
    { LOG_WARN("MinimapKey equals ToggleKey (%s); MinimapKey disabled.", VKToName(minimap).c_str()); minimap = 0; }
    if (reload && (reload == toggle || reload == minimap))
    { LOG_WARN("ReloadKey duplicates another key (%s); ReloadKey disabled.", VKToName(reload).c_str()); reload = 0; }

    c.autoShowWhenAiming = ini.GetBool("Behavior", "AutoShowWhenAiming", false);
    c.showOnLowHealth    = ini.GetBool("Behavior", "ShowOnLowHealth",    false);
    c.lowHealthPercent   = ini.GetInt ("Behavior", "LowHealthPercent",   25, 1, 90);
    c.lowHealthSeconds   = ini.GetInt ("Behavior", "LowHealthSeconds",   4,  1, 30);
    c.toggleMessage      = ini.GetBool("Behavior", "ToggleMessage",      true);
    c.resetOnDeath       = ini.GetBool("Behavior", "ResetOnDeath",       true);

    const std::string mode = Lower(ini.Get("Advanced", "HideMode", "Full"));
    if (mode == "soft")      c.mode = HideMode::Soft;
    else if (mode == "full") c.mode = HideMode::Full;
    else { LOG_WARN("HideMode '%s' is invalid, using Full.", mode.c_str()); c.mode = HideMode::Full; }

    const std::string dot = Lower(ini.Get("Advanced", "DotMode", "Aiming"));
    if (dot == "off")          c.dotMode = DotMode::Off;
    else if (dot == "aiming")  c.dotMode = DotMode::Aiming;
    else if (dot == "always")  c.dotMode = DotMode::Always;
    else { LOG_WARN("DotMode '%s' is invalid, using Aiming.", dot.c_str()); c.dotMode = DotMode::Aiming; }
    c.dotSize  = ini.GetInt("Advanced", "DotSize",  3,   1, 20);
    c.dotAlpha = ini.GetInt("Advanced", "DotAlpha", 170, 0, 255);
    c.clearHelpText   = ini.GetBool("Advanced", "ClearHelpText",   true);
    ParseExtraContexts(ini.Get("Advanced", "ExtraContexts"), c.extraContexts);
    ParsePeekControls(ini.Get("Advanced", "PeekWhileHeld", "WeaponWheel"), c.peekControls);
    for (uint32_t h : c.peekControls) if (h == CTRL_OPEN_WHEEL_MENU) c.peekTabKey = true;

    // --- Controller -------------------------------------------------------
    c.padEnabled = ini.GetBool("Controller", "Enabled", true);
    {
        const std::string idx = Lower(Trim(ini.Get("Controller", "ControllerIndex", "Auto")));
        if (idx == "auto" || idx.empty()) c.padIndex = -1;
        else c.padIndex = ini.GetInt("Controller", "ControllerIndex", 0, 1, 4) - 1;  // invalid -> default 0 -> Auto
    }
    c.padToggle  = ResolvePadCombo(ini, "ToggleButton",  "LB+DPadDown");
    c.padMinimap = ResolvePadCombo(ini, "MinimapButton", nullptr);
    c.padReload  = ResolvePadCombo(ini, "ReloadButton",  nullptr);
    if (c.padMinimap.Valid() && c.padMinimap == c.padToggle)
    { LOG_WARN("MinimapButton equals ToggleButton (%s); MinimapButton disabled.", PadComboName(c.padMinimap).c_str()); c.padMinimap = PadCombo{}; }
    if (c.padReload.Valid() && (c.padReload == c.padToggle || c.padReload == c.padMinimap))
    { LOG_WARN("ReloadButton duplicates another button combo (%s); ReloadButton disabled.", PadComboName(c.padReload).c_str()); c.padReload = PadCombo{}; }
    if (c.padEnabled)
    {
        const PadCombo* all[3] = { &c.padToggle, &c.padMinimap, &c.padReload };
        for (const PadCombo* pc : all)
            if (pc->Valid() && pc->Count() == 1)
                LOG_WARN("Controller combo '%s' is a single button: it will also do its normal in-game action.", PadComboName(*pc).c_str());
    }

    g_cfg = c;
    g_vkToggle.store(toggle);
    g_vkMinimap.store(minimap);
    g_vkReload.store(reload);

    LOG_INFO("Keys: Toggle=%s Minimap=%s Reload=%s | Mode=%s AutoShowAiming=%d LowHealth=%d(%d%%,%ds) Message=%d",
             VKToName(toggle).c_str(), VKToName(minimap).c_str(), VKToName(reload).c_str(),
             c.mode == HideMode::Full ? "Full" : "Soft", c.autoShowWhenAiming,
             c.showOnLowHealth, c.lowHealthPercent, c.lowHealthSeconds, c.toggleMessage);
    LOG_INFO("Controller: %s | Pad=%s Toggle=%s Minimap=%s Reload=%s",
             c.padEnabled ? "enabled" : "disabled",
             c.padIndex < 0 ? "Auto" : std::to_string(c.padIndex + 1).c_str(),
             PadComboName(c.padToggle).c_str(), PadComboName(c.padMinimap).c_str(),
             PadComboName(c.padReload).c_str());
}

// -----------------------------------------------------------------------------
//  Keyboard input  -  edge detection
//  ScriptHook calls this on the window thread for every key message.
//  We only record "this key went down" (never while held) and let the script
//  thread act on it, because natives may only be called from the script thread.
// -----------------------------------------------------------------------------

static std::atomic<bool> g_pendingToggle{ false };
static std::atomic<bool> g_pendingMinimap{ false };
static std::atomic<bool> g_pendingReload{ false };

static void OnKeyboardMessage(DWORD key, WORD /*repeats*/, BYTE /*scanCode*/,
                              BOOL /*isExtended*/, BOOL isWithAlt,
                              BOOL wasDownBefore, BOOL isUpNow)
{
    if (isUpNow)        return;   // key release
    if (wasDownBefore)  return;   // auto-repeat while held -> not a new press
    if (isWithAlt)      return;   // Alt+F4 and friends

    const int k = static_cast<int>(key);
    if (k == g_vkToggle.load())                         g_pendingToggle.store(true);
    else if (g_vkMinimap.load() && k == g_vkMinimap.load()) g_pendingMinimap.store(true);
    else if (g_vkReload.load()  && k == g_vkReload.load())  g_pendingReload.store(true);
}

// -----------------------------------------------------------------------------
//  Controller input  -  polled once per frame on the script thread
//  A combo fires once when ALL of its buttons become held (edge detection).
//  If several combos fire in the same frame, the one with the most buttons wins
//  (so "DPadDown" and "LB+DPadDown" can coexist).
// -----------------------------------------------------------------------------

static bool GameWindowFocused()
{
    DWORD pid = 0;
    if (HWND fg = GetForegroundWindow()) GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

static bool PadComboDown(const PadCombo& c, const XINPUT_GAMEPAD& g)
{
    if (!c.Valid()) return false;
    if ((g.wButtons & c.buttons) != c.buttons) return false;
    if (c.lt && g.bLeftTrigger  < kPadTriggerThreshold) return false;
    if (c.rt && g.bRightTrigger < kPadTriggerThreshold) return false;
    return true;
}

static void PollPad(ULONGLONG now)
{
    static bool      prevDown[3]  = { false, false, false };
    static bool      connected[4] = { false, false, false, false };
    static ULONGLONG nextProbe[4] = { 0, 0, 0, 0 };   // disconnected slots are only re-probed every 1 s

    const PadCombo* combos[3] = { &g_cfg.padToggle, &g_cfg.padMinimap, &g_cfg.padReload };
    bool down[3] = { false, false, false };

    if (g_cfg.padEnabled && GameWindowFocused())   // ignore the pad while alt-tabbed
    {
        for (int slot = 0; slot < 4; ++slot)
        {
            if (g_cfg.padIndex >= 0 && slot != g_cfg.padIndex) continue;
            if (!connected[slot] && now < nextProbe[slot]) continue;

            XINPUT_STATE st{};
            if (XInputGetState(static_cast<DWORD>(slot), &st) != ERROR_SUCCESS)
            {
                if (connected[slot]) LOG_INFO("Controller %d disconnected.", slot + 1);
                connected[slot] = false;
                nextProbe[slot] = now + 1000;
                continue;
            }
            if (!connected[slot]) { connected[slot] = true; LOG_INFO("Controller %d connected.", slot + 1); }

            for (int i = 0; i < 3; ++i)
                if (PadComboDown(*combos[i], st.Gamepad)) down[i] = true;
        }
    }

    int best = -1, bestCount = 0;
    for (int i = 0; i < 3; ++i)
    {
        if (down[i] && !prevDown[i] && combos[i]->Count() > bestCount)
        { best = i; bestCount = combos[i]->Count(); }
        prevDown[i] = down[i];
    }

    if      (best == 0) g_pendingToggle.store(true);
    else if (best == 1) g_pendingMinimap.store(true);
    else if (best == 2) g_pendingReload.store(true);
}

// -----------------------------------------------------------------------------
//  Runtime state
// -----------------------------------------------------------------------------

static ULONGLONG g_peekUntil     = 0;      // HUD stays visible until this tick (button held)
static bool      g_userHidden    = false;  // user wants the whole HUD hidden
static bool      g_minimapOnly   = false;  // user wants only the minimap hidden
static ULONGLONG g_resumeAt      = 0;      // no hiding before this tick (after cutscene etc.)

// Ownership + snapshot, so restoring returns the EXACT previous state.
static bool g_ownHud    = false;  static bool g_hudWasHidden   = false;
static bool g_ownRadar  = false;  static bool g_radarWasHidden = false;

// Low-health pulse
static bool      g_lowLatched = false;
static ULONGLONG g_lowUntil   = 0;

// Toggle message
static char      g_toast[48]  = "";
static ULONGLONG g_toastUntil = 0;

static void ShowToast(const char* text, ULONGLONG now)
{
    if (!g_cfg.toggleMessage) return;
    std::snprintf(g_toast, sizeof(g_toast), "%s", text);
    g_toastUntil = now + kToastMs;
}

static void DrawToast(ULONGLONG now)
{
    if (now >= g_toastUntil) return;

    const ULONGLONG left = g_toastUntil - now;
    const int alpha = left < kToastFadeMs ? static_cast<int>(255 * left / kToastFadeMs) : 255;

    static char tpl[] = "LITERAL_STRING";
    UI::SET_TEXT_SCALE(0.0f, 0.40f);
    UI::SET_TEXT_COLOR_RGBA(255, 255, 255, alpha);
    UI::SET_TEXT_CENTRE(TRUE);
    UI::SET_TEXT_DROPSHADOW(2, 0, 0, 0, alpha);
    UI::DRAW_TEXT(GAMEPLAY::CREATE_STRING(10, tpl, g_toast), 0.5f, 0.92f);
}

// -----------------------------------------------------------------------------
//  Game-state queries
// -----------------------------------------------------------------------------

struct GameState
{
    bool loading;     // loading screen
    bool faded;       // screen faded / fading out (mission transitions)
    bool dead;        // dead, dying or not "playing" (respawn)
    bool noControl;   // cutscene / scripted sequence / scripted camera
    bool aiming;      // free aim, lock-on or first-person aim
    bool menuOpen;    // pause menu or text chat
    bool peek;        // a PeekWhileHeld game button is held
};

static GameState QueryGameState(Player player, Ped ped)
{
    GameState s{};
    s.loading   = DLC2::GET_IS_LOADING_SCREEN_ACTIVE() != 0;
    s.faded     = CAM::IS_SCREEN_FADED_OUT() || CAM::IS_SCREEN_FADING_OUT();
    s.dead      = ped <= 0 || PLAYER::IS_PLAYER_DEAD(player) || !PLAYER::IS_PLAYER_PLAYING(player) ||
                  PED::IS_PED_DEAD_OR_DYING(ped, TRUE);
    s.noControl = !PLAYER::IS_PLAYER_CONTROL_ON(player) || !CAM::IS_GAMEPLAY_CAM_RENDERING();
    s.aiming    = PLAYER::IS_PLAYER_FREE_AIMING(player) || PLAYER::IS_PLAYER_TARGETTING_ANYTHING(player);
    s.menuOpen  = UI::IS_PAUSE_MENU_ACTIVE() || UI::_IS_TEXT_CHAT_ACTIVE();
    // Weapon wheel: also watch the physical Tab key (default binding), in case the
    // game stops reporting the wheel control while the HUD is hidden.
    if (g_cfg.peekTabKey && (GetAsyncKeyState(0x09 /*VK_TAB*/) & 0x8000)) s.peek = true;
    for (uint32_t h : g_cfg.peekControls)
        if (CONTROLS::IS_CONTROL_PRESSED(0, static_cast<int>(h)) ||
            CONTROLS::IS_DISABLED_CONTROL_PRESSED(0, static_cast<int>(h)))
        { s.peek = true; break; }
    return s;
}

// -----------------------------------------------------------------------------
//  Applying / restoring
// -----------------------------------------------------------------------------

// Whole HUD (cores, ammo, compass, prompts, money, feed, horse HUD ...).
// The reticle is NOT hidden on purpose by any call below other than DISPLAY_HUD;
// see the header comment about HideMode=Soft if DISPLAY_HUD affects it.
static void ApplyHud(bool hide)
{
    if (hide)
    {
        if (!g_ownHud)
        {
            g_hudWasHidden = UI::IS_HUD_HIDDEN() != 0;   // remember exact prior state
            g_ownHud = true;
            LOG_INFO("HUD hide (previous state: %s).", g_hudWasHidden ? "hidden" : "visible");
        }
        if (!UI::IS_HUD_HIDDEN()) UI::DISPLAY_HUD(FALSE);  // game re-enabled it -> hide again
    }
    else if (g_ownHud)
    {
        UI::DISPLAY_HUD(g_hudWasHidden ? FALSE : TRUE);
        g_ownHud = false;
        LOG_INFO("HUD restored.");
    }
}

static void ApplyRadar(bool hide)
{
    if (hide)
    {
        if (!g_ownRadar)
        {
            g_radarWasHidden = UI::IS_RADAR_HIDDEN() != 0;
            g_ownRadar = true;
        }
        if (!UI::IS_RADAR_HIDDEN()) RADAR::DISPLAY_RADAR(FALSE);
    }
    else if (g_ownRadar)
    {
        RADAR::DISPLAY_RADAR(g_radarWasHidden ? FALSE : TRUE);
        g_ownRadar = false;
    }
}

// Contexts are "this frame" only: nothing to undo, and if the script is
// unloaded they simply stop being applied.
static void EnableContextThisFrame(uint32_t hash)
{
    invoke<Void>(NATIVE_ENABLE_HUD_CONTEXT_THIS_FRAME, static_cast<Hash>(hash));
}

static void ApplyContexts(bool hudHidden, bool radarHidden)
{
    if (hudHidden)
    {
        EnableContextThisFrame(CTX_INFINITE_AMMO);                  // ammo counter
        for (uint32_t h : g_cfg.extraContexts) EnableContextThisFrame(h);
    }
    if (radarHidden)
        EnableContextThisFrame(CTX_RADAR_FORCE_HIDE);               // minimap
}

// Filled circle made of 1-pixel-high rows, centered on the screen center.
static void DrawDisc(float diameterPx, int w, int h, int r, int g, int b, int a)
{
    const int   d   = (std::max)(1, static_cast<int>(std::lround(diameterPx)));
    const float rad = d * 0.5f;
    for (int i = 0; i < d; ++i)
    {
        const float dy   = (i + 0.5f) - rad;
        const float half = std::sqrt((std::max)(0.0f, rad * rad - dy * dy));
        const float wpx  = (std::max)(1.0f, std::round(2.0f * half));
        GRAPHICS::DRAW_RECT(0.5f, 0.5f + dy / h, wpx / w, 1.0f / h, r, g, b, a, FALSE, FALSE);
    }
}

// The game's own reticle disappears with DISPLAY_HUD(false), so we draw a small,
// subtle round dot ourselves: faint dark halo underneath, soft white dot on top.
static void DrawCenterDot()
{
    int w = 0, h = 0;
    GRAPHICS::GET_SCREEN_RESOLUTION(&w, &h);
    if (w <= 0 || h <= 0) { w = 1920; h = 1080; }

    const float d = static_cast<float>(g_cfg.dotSize);
    DrawDisc(d + 2.0f, w, h, 0, 0, 0, g_cfg.dotAlpha * 35 / 100);
    DrawDisc(d,        w, h, 235, 235, 235, g_cfg.dotAlpha);
}

// -----------------------------------------------------------------------------
//  Input handling (script thread)
// -----------------------------------------------------------------------------

static void ConsumeInput(ULONGLONG now, const GameState& gs)
{
    const bool t = g_pendingToggle.exchange(false);
    const bool m = g_pendingMinimap.exchange(false);
    const bool r = g_pendingReload.exchange(false);
    if (!(t || m || r)) return;

    // Ignore presses while a menu / text input is open (discarded, not queued).
    if (gs.menuOpen) { LOG_INFO("Input ignored: menu or text input open."); return; }

    if (r)
    {
        LoadConfig(true);
        ShowToast("Clean HUD: config reloaded", now);
    }
    if (t)
    {
        g_userHidden = !g_userHidden;
        g_lowLatched = false; g_lowUntil = 0;
        LOG_INFO("Toggle -> HUD %s.", g_userHidden ? "hidden" : "shown");
        ShowToast(g_userHidden ? "HUD Hidden" : "HUD Shown", now);
    }
    if (m)
    {
        g_minimapOnly = !g_minimapOnly;
        LOG_INFO("Minimap-only -> %s.", g_minimapOnly ? "hidden" : "shown");
        ShowToast(g_minimapOnly ? "Minimap Hidden" : "Minimap Shown", now);
    }
}

// Returns true while the low-health pulse wants the HUD visible.
static bool UpdateLowHealth(ULONGLONG now, Ped ped, bool suspended)
{
    if (!g_cfg.showOnLowHealth || !g_userHidden)
    {
        g_lowLatched = false; g_lowUntil = 0;
        return false;
    }
    if (suspended) return false;

    const int hp = ENTITY::GET_ENTITY_HEALTH(ped);
    const int mx = ENTITY::GET_ENTITY_MAX_HEALTH(ped, FALSE);
    if (mx > 0)
    {
        const float pct = 100.0f * static_cast<float>(hp) / static_cast<float>(mx);
        if (pct < static_cast<float>(g_cfg.lowHealthPercent))
        {
            if (!g_lowLatched)
            {
                g_lowLatched = true;
                g_lowUntil   = now + static_cast<ULONGLONG>(g_cfg.lowHealthSeconds) * 1000;
                LOG_INFO("Low health (%.0f%%): showing HUD for %d s.", pct, g_cfg.lowHealthSeconds);
            }
        }
        else if (pct >= static_cast<float>(g_cfg.lowHealthPercent) + 10.0f)
            g_lowLatched = false;   // recovered -> re-arm
    }
    return now < g_lowUntil;
}

// -----------------------------------------------------------------------------
//  One frame
// -----------------------------------------------------------------------------

static void Tick()
{
    const ULONGLONG now    = GetTickCount64();
    const Player    player = PLAYER::PLAYER_ID();
    const Ped       ped    = PLAYER::PLAYER_PED_ID();
    const GameState gs     = QueryGameState(player, ped);

    PollPad(now);
    ConsumeInput(now, gs);

    // --- Safety: never leave the HUD stuck hidden --------------------------
    if (gs.loading && (g_userHidden || g_minimapOnly))
    {
        LOG_INFO("Loading screen / transition: HUD state reset to visible.");
        g_userHidden = false; g_minimapOnly = false;
    }
    if (gs.dead && g_cfg.resetOnDeath && g_userHidden)
    {
        LOG_INFO("Player died: HUD state reset to visible.");
        g_userHidden = false;
    }

    // Cutscenes, fades and death/respawn: restore the game's own state and
    // wait kResumeDelayMs after they end before hiding again.
    const bool unsafeNow = gs.loading || gs.faded || gs.dead || gs.noControl;
    if (unsafeNow) g_resumeAt = now + kResumeDelayMs;
    const bool suspended = unsafeNow || now < g_resumeAt;

    // --- Decide what should be hidden this frame --------------------------
    const bool lowHealthShow = UpdateLowHealth(now, ped, suspended);
    if (gs.peek) g_peekUntil = now + kPeekLingerMs;
    const bool peeking = now < g_peekUntil;

    const bool hideAll = g_userHidden && !suspended && !lowHealthShow && !peeking &&
                         !(g_cfg.autoShowWhenAiming && gs.aiming);
    // While peeking (e.g. weapon wheel) the minimap stays hidden.
    const bool radarWanted = g_minimapOnly || (g_userHidden && peeking);
    const bool hideRadar = hideAll || (radarWanted && !suspended);

    // --- Apply (every frame while active) ---------------------------------
    ApplyHud(hideAll && g_cfg.mode == HideMode::Full);
    ApplyRadar(hideRadar);
    ApplyContexts(hideAll, hideRadar);

    if (hideAll && g_cfg.clearHelpText)
        UI::CLEAR_ALL_HELP_MESSAGES();

    if (hideAll && g_cfg.mode == HideMode::Full &&
        (g_cfg.dotMode == DotMode::Always || (g_cfg.dotMode == DotMode::Aiming && gs.aiming)))
        DrawCenterDot();

    DrawToast(now);
}

// -----------------------------------------------------------------------------
//  Script entry point  -  single main loop
// -----------------------------------------------------------------------------

static void ScriptMain()
{
    LOG_INFO("Clean HUD script started.");
    LoadConfig(false);

    bool faultLogged = false;
    while (true)
    {
        try { Tick(); }
        catch (...)
        {
            if (!faultLogged) { LOG_WARN("Unexpected exception in main loop (further ones suppressed)."); faultLogged = true; }
        }
        WAIT(0);
    }
}

// -----------------------------------------------------------------------------
//  DLL entry point
// -----------------------------------------------------------------------------

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID /*reserved*/)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
    {
        DisableThreadLibraryCalls(hModule);
        g_module = hModule;

        char path[MAX_PATH] = "";
        GetModuleFileNameA(hModule, path, MAX_PATH);
        g_dir = path;
        g_dir = g_dir.substr(0, g_dir.find_last_of("\\/") + 1);

        g_logPath = g_dir + "CleanHUD.log";
        if (FILE* f = fopen(g_logPath.c_str(), "w"))   // fresh log each session
        {
            fprintf(f, "Clean HUD log\n");
            fclose(f);
        }

        keyboardHandlerRegister(OnKeyboardMessage);
        scriptRegister(hModule, ScriptMain);
        break;
    }
    case DLL_PROCESS_DETACH:
        keyboardHandlerUnregister(OnKeyboardMessage);
        scriptUnregister(hModule);
        break;
    }
    return TRUE;
}
