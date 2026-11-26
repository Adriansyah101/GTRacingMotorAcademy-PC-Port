#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "compat.h"

#include "rt.h"

#define INI_LINE 512
#define INI_KEY_MAX 64
#define INI_KEYS_MAX 128

static char ini_path[MAX_PATH * 2];
static char ini_keys[INI_KEYS_MAX][INI_KEY_MAX];
static int n_ini_keys;
static char notes[8192];
static size_t notes_n;

const char *gt_ini_path(void) { return ini_path; }

void gt_ini_forget_env(void)
{
    for (int i = 0; i < n_ini_keys; i++)
        _putenv_s(ini_keys[i], "");
}

static void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void note(const char *fmt, ...)
{
    if (notes_n >= sizeof notes - 1)
        return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(notes + notes_n, sizeof notes - notes_n, fmt, ap);
    va_end(ap);
    if (n > 0)
        notes_n += (size_t)n < sizeof notes - notes_n ? (size_t)n : sizeof notes - 1 - notes_n;
}

static void remember(const char *k)
{
    if (n_ini_keys < INI_KEYS_MAX)
        snprintf(ini_keys[n_ini_keys++], INI_KEY_MAX, "%s", k);
    else
        note("[ini] %s: more than %d keys, not forgotten on a Display menu restart\n", k, INI_KEYS_MAX);
}

static int is_address(const char *k)
{
    size_t kn = strlen(k);
    return kn > 4 && !_stricmp(k + kn - 4, "_URL");
}

static int key_ok(const char *k)
{
    if (!*k || strlen(k) >= INI_KEY_MAX)
        return 0;
    for (; *k; k++)
        if (!(*k == '_' || (*k >= 'A' && *k <= 'Z') || (*k >= 'a' && *k <= 'z') || (*k >= '0' && *k <= '9')))
            return 0;
    return 1;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    for (char *e = s + strlen(s); e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t');)
        *--e = 0;
    return s;
}

static void load_file(const char *exe_dir)
{
    char line[INI_LINE];
    const char *alt = getenv("GT_INI");
    if (alt && *alt)
        snprintf(ini_path, sizeof ini_path, "%s", alt);
    else
        snprintf(ini_path, sizeof ini_path, "%s" GT_SEP "gtracing.ini", exe_dir);
    FILE *f = fopen(ini_path, "r");
    if (!f)
        return;
    for (int first = 1; fgets(line, sizeof line, f); first = 0) {
        size_t len = strlen(line);
        if (len == sizeof line - 1 && line[len - 1] != '\n') {
            int ch = fgetc(f);
            if (ch != EOF) {
                while (ch != EOF && ch != '\n')
                    ch = fgetc(f);
                note("[ini] a line over %d characters skipped\n", INI_LINE - 2);
                continue;
            }
        }
        char *k = line, *v;
        if (first && !memcmp(k, "\xef\xbb\xbf", 3))
            k += 3;
        k = trim(k);
        if (*k == '#' || *k == ';' || !(v = strchr(k, '=')))
            continue;
        *v++ = 0;
        k = trim(k);
        v = trim(v);
        if (!*k)
            continue;
        if (!key_ok(k)) {
            note("[ini] a key that is not a name (letters, digits, _) skipped\n");
            continue;
        }
        int hide = is_address(k);
        if (getenv(k))
            note("[ini] %s: kept the environment's %s\n", k, hide ? "(an address, not logged)" : getenv(k));
        else if (*v) {
            _putenv_s(k, v);
            note("[ini] %s=%s\n", k, hide ? "(an address, not logged)" : v);
            remember(k);
        }
    }
    fclose(f);
}

static void load_defaults(void)
{
    static const char *const defaults[][2] = {
        {"GT_DEVICE", "ipad3"}, {"GT_RESOLUTION", "monitor"}, {"GT_WIDE", "monitor"},
        {"GT_FULLSCREEN", "1"}, {"GT_MSAA", "auto"},
        {"GT_POSTFX", "4"},
        {"GT_CLOUD_BOARD_URL", "https://gt-redbull-board.gtracing-pc.workers.dev"},
    };
    for (size_t i = 0; i < sizeof defaults / sizeof defaults[0]; i++) {
        const char *k = defaults[i][0], *v = getenv(k);
        if (v && *v)
            continue;
        _putenv_s(k, defaults[i][1]);
        note("[ini] %s=%s (the default)\n", k, is_address(k) ? "(an address, not logged)" : defaults[i][1]);
        remember(k);
    }
}

void gt_settings_load(const char *exe_dir)
{
    load_file(exe_dir);
    load_defaults();
}

void gt_settings_log(void)
{
    for (char *p = notes, *e; *p; p = e + 1) {
        if (!(e = strchr(p, '\n')))
            break;
        rt_log("%.*s\n", (int)(e - p), p);
    }
    SecureZeroMemory(notes, sizeof notes);
    notes_n = 0;
}

int rt_parse_int(const char *s, int def, int lo, int hi)
{
    if (!s)
        return def;
    char *end;
    while (*s == ' ' || *s == '\t')
        s++;
    long long v = strtoll(s, &end, 10);
    while (*end == ' ' || *end == '\t')
        end++;
    if (end == s || *end)
        return def;
    return v < lo ? lo : v > hi ? hi : (int)v;
}

float rt_parse_float(const char *s, float def, float lo, float hi)
{
    if (!s)
        return def;
    char *end;
    while (*s == ' ' || *s == '\t')
        s++;
    double v = strtod(s, &end);
    while (*end == ' ' || *end == '\t')
        end++;
    if (end == s || *end || !isfinite(v))
        return def;
    return v < lo ? lo : v > hi ? hi : (float)v;
}

int rt_setting_int(const char *key, int def, int lo, int hi) { return rt_parse_int(getenv(key), def, lo, hi); }
float rt_setting_float(const char *key, float def, float lo, float hi)
{
    return rt_parse_float(getenv(key), def, lo, hi);
}
