/*
 * Read-only NitroSDK ROM archive backed by portable libpak.
 *
 * In v1 the pack contains sparse NitroFS IDs, sizes and payloads, but no
 * filenames. The player's manifest.json is loaded as a small sidecar to
 * map paths to IDs. Neither ROM nor whole pack is ever loaded into RAM.
 *
 * The SDK command processor retains FSFile's own position/length fields;
 * FS_SeekFile and FS_GetLength thus keep their original SDK behaviour.
 * FS_LoadOverlayImage uses the same FS_OpenFileFast + FS_ReadFile path.
 */
#include <nitro_port_fs.h>

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MANIFEST_MAX_BYTES (2u * 1024u * 1024u)
#define PATH_MAX_BYTES 1024u

typedef struct {
    char *path;
    u32 id;
    u32 size;
} RomPath;

static Pak *sPak;
static RomPath *sPaths;
static size_t sPathCount;

static void free_paths(RomPath *paths, size_t count)
{
    size_t i;
    if (!paths) return;
    for (i = 0; i < count; ++i) free(paths[i].path);
    free(paths);
}

void NitroPort_ClosePak(void)
{
    pak_close(sPak);
    sPak = NULL;
    free_paths(sPaths, sPathCount);
    sPaths = NULL;
    sPathCount = 0;
}

BOOL NitroPort_IsPakOpen(void)
{
    return sPak != NULL ? TRUE : FALSE;
}

static const char *space(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    return p;
}

static int expect(const char **p, char ch)
{
    *p = space(*p);
    if (**p != ch) return 0;
    ++*p;
    return 1;
}

/* Parse only JSON strings representable as ASCII paths/keys, as generated
 * by the builder. Escaped ASCII and \u00XX escapes are handled as well. */
static int parse_string(const char **p, char **out)
{
    char *buf;
    size_t len = 0;
    const char *s;
    if (!expect(p, '"')) return 0;
    s = *p;
    buf = (char *)malloc(PATH_MAX_BYTES + 1);
    if (!buf) return 0;
    while (*s && *s != '"') {
        unsigned int c = (unsigned char)*s++;
        if (c == '\\') {
            if (!*s) goto invalid;
            c = (unsigned char)*s++;
            if (c == '"' || c == '\\' || c == '/') {
                /* literal escaped character */
            } else if (c == 'u') {
                int n;
                c = 0;
                for (n = 0; n < 4; ++n) {
                    unsigned int digit;
                    if (!*s) goto invalid;
                    digit = (unsigned char)*s++;
                    if (digit >= '0' && digit <= '9') digit -= '0';
                    else if (digit >= 'a' && digit <= 'f') digit = digit - 'a' + 10;
                    else if (digit >= 'A' && digit <= 'F') digit = digit - 'A' + 10;
                    else goto invalid;
                    c = (c << 4) | digit;
                }
            } else {
                goto invalid;
            }
        }
        if (c < 32 || c > 127 || len >= PATH_MAX_BYTES) goto invalid;
        buf[len++] = (char)c;
    }
    if (*s != '"') goto invalid;
    buf[len] = '\0';
    *p = s + 1;
    *out = buf;
    return 1;
invalid:
    free(buf);
    return 0;
}

static int parse_u32(const char **p, u32 *out)
{
    unsigned long long value = 0;
    const char *s = space(*p);
    if (*s < '0' || *s > '9') return 0;
    if (*s == '0' && s[1] >= '0' && s[1] <= '9') return 0;
    do {
        value = value * 10 + (unsigned int)(*s++ - '0');
        if (value > UINT32_MAX) return 0;
    } while (*s >= '0' && *s <= '9');
    *out = (u32)value;
    *p = s;
    return 1;
}

static int safe_path(const char *path)
{
    const char *p = path;
    if (!*p || *p == '/' || *p == '\\') return 0;
    while (*p) {
        const char *component = p;
        size_t len;
        while (*p && *p != '/') {
            if (*p == '\\' || *p == ':' || (unsigned char)*p < 32) return 0;
            ++p;
        }
        len = (size_t)(p - component);
        if (len == 0 || (len == 1 && component[0] == '.') ||
            (len == 2 && component[0] == '.' && component[1] == '.')) return 0;
        if (*p == '/') ++p;
        if (*p == '\0' && p[-1] == '/') return 0;
    }
    return 1;
}

static int parse_entry(const char **p, RomPath *out)
{
    unsigned int fields = 0;
    if (!expect(p, '{')) return 0;
    memset(out, 0, sizeof(*out));
    do {
        char *key = NULL;
        if (!parse_string(p, &key) || !expect(p, ':')) {
            free(key);
            return 0;
        }
        if (strcmp(key, "id") == 0 && !(fields & 1u)) {
            fields |= 1u;
            if (!parse_u32(p, &out->id)) { free(key); return 0; }
        } else if (strcmp(key, "path") == 0 && !(fields & 2u)) {
            fields |= 2u;
            if (!parse_string(p, &out->path)) { free(key); return 0; }
        } else if (strcmp(key, "size") == 0 && !(fields & 4u)) {
            fields |= 4u;
            if (!parse_u32(p, &out->size)) { free(key); return 0; }
        } else {
            free(key);
            return 0;
        }
        free(key);
        *p = space(*p);
        if (**p != ',') break;
        ++*p;
    } while (1);
    return fields == 7u && expect(p, '}') && safe_path(out->path);
}

static int equal_path(const char *candidate, const char *wanted)
{
    while (*wanted == '/' || *wanted == '\\') ++wanted;
    while (*candidate && *wanted) {
        unsigned int a = (unsigned char)*candidate++;
        unsigned int b = (unsigned char)*wanted++;
        if (b == '\\') b = '/';
        if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
        if (a != b) return 0;
    }
    return !*candidate && !*wanted;
}

static PakResult load_paths(const char *manifest_path, Pak *pak,
                            RomPath **out_paths, size_t *out_count)
{
    FILE *file;
    char *json = NULL;
    long length;
    const char *p;
    RomPath *paths = NULL;
    size_t count = 0, capacity = 0, i;
    PakResult result = PAK_ERR_FORMAT;

    *out_paths = NULL;
    *out_count = 0;
    file = fopen(manifest_path, "rb");
    if (!file) return PAK_ERR_IO;
    if (fseek(file, 0, SEEK_END) || (length = ftell(file)) < 0 ||
        (unsigned long)length > MANIFEST_MAX_BYTES ||
        fseek(file, 0, SEEK_SET)) goto done;
    json = (char *)malloc((size_t)length + 1u);
    if (!json) { result = PAK_ERR_MEMORY; goto done; }
    if (fread(json, 1, (size_t)length, file) != (size_t)length) {
        result = PAK_ERR_IO;
        goto done;
    }
    json[length] = '\0';

    /* The builder writes a top-level "files" array; look only for that
     * literal field and validate every object, not arbitrary text lines. */
    p = strstr(json, "\"files\"");
    if (!p) goto done;
    p += sizeof("\"files\"") - 1;
    if (!expect(&p, ':') || !expect(&p, '[')) goto done;
    p = space(p);
    while (*p != ']') {
        RomPath entry;
        RomPath *new_paths;
        u32 size;
        if (!parse_entry(&p, &entry)) {
            free(entry.path);
            goto done;
        }
        if (pak_find(pak, entry.id, &size) != PAK_OK || size != entry.size) {
            free(entry.path);
            goto done;
        }
        for (i = 0; i < count; ++i) {
            if (paths[i].id == entry.id || equal_path(paths[i].path, entry.path)) {
                free(entry.path);
                goto done;
            }
        }
        if (count == capacity) {
            size_t next = capacity ? capacity * 2u : 32u;
            if (next > MANIFEST_MAX_BYTES / sizeof(RomPath)) {
                free(entry.path);
                goto done;
            }
            new_paths = (RomPath *)realloc(paths, next * sizeof(*paths));
            if (!new_paths) {
                free(entry.path);
                result = PAK_ERR_MEMORY;
                goto done;
            }
            paths = new_paths;
            capacity = next;
        }
        paths[count++] = entry;
        p = space(p);
        if (*p == ']') break;
        if (!expect(&p, ',')) goto done;
    }
    if (!expect(&p, ']')) goto done;
    result = PAK_OK;
    *out_paths = paths;
    *out_count = count;
    paths = NULL;
    count = 0;
done:
    fclose(file);
    free(json);
    free_paths(paths, count);
    return result;
}

PakResult NitroPort_OpenPak(const char *pak_path, const char *manifest_path,
                            const char *expected_build_id)
{
    Pak *pak = NULL;
    RomPath *paths = NULL;
    size_t count = 0;
    PakResult result;
    if (!manifest_path) return PAK_ERR_ARGUMENT;
    /* Keep the old pack on failure, so a typo cannot break a working mount. */
    result = pak_open(pak_path, expected_build_id, &pak);
    if (result != PAK_OK) return result;
    result = load_paths(manifest_path, pak, &paths, &count);
    if (result != PAK_OK) {
        pak_close(pak);
        return result;
    }
    NitroPort_ClosePak();
    sPak = pak;
    sPaths = paths;
    sPathCount = count;
    return PAK_OK;
}

FSResult NitroPort_PakArchiveProc(FSFile *file, FSCommandType command)
{
    size_t i;
    u32 size;
    if (command == FS_COMMAND_ACTIVATE || command == FS_COMMAND_IDLE ||
        command == FS_COMMAND_SUSPEND || command == FS_COMMAND_RESUME)
        return FS_RESULT_SUCCESS;
    if (!sPak || !file) return FS_RESULT_FAILURE;
    switch (command) {
    case FS_COMMAND_FINDPATH:
        if (file->arg.findpath.find_directory ||
            !file->arg.findpath.path || !file->arg.findpath.result.file)
            return FS_RESULT_FAILURE;
        for (i = 0; i < sPathCount; ++i) {
            if (equal_path(sPaths[i].path, file->arg.findpath.path)) {
                file->arg.findpath.result.file->arc = file->arc;
                file->arg.findpath.result.file->file_id = sPaths[i].id;
                return FS_RESULT_SUCCESS;
            }
        }
        return FS_RESULT_FAILURE;
    case FS_COMMAND_OPENFILEFAST:
        if (pak_find(sPak, file->arg.openfilefast.id.file_id, &size) != PAK_OK)
            return FS_RESULT_FAILURE;
        file->prop.file.own_id = file->arg.openfilefast.id.file_id;
        file->prop.file.top = 0;
        file->prop.file.pos = 0;
        file->prop.file.bottom = size;
        return FS_RESULT_SUCCESS;
    case FS_COMMAND_READFILE: {
        u32 position = file->prop.file.pos;
        u32 length = file->arg.readfile.len;
        if (pak_read(sPak, file->prop.file.own_id, position,
                     file->arg.readfile.dst, length) != PAK_OK)
            return FS_RESULT_FAILURE;
        file->prop.file.pos = position + length;
        return FS_RESULT_SUCCESS;
    }
    case FS_COMMAND_CLOSEFILE:
        return FS_RESULT_SUCCESS;
    case FS_COMMAND_WRITEFILE:
        return FS_RESULT_UNSUPPORTED;
    default:
        /* A v1 pack contains neither an FNT directory table nor overlay
         * table offsets. Direct ROM-offset reads must never return junk. */
        return FS_RESULT_FAILURE;
    }
}
