/*
 * Copyright 2017 Hugh McMaster
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <windows.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include <wine/unicode.h>
#include <wine/debug.h>

#include "reg.h"

WINE_DEFAULT_DEBUG_CHANNEL(reg);

static WCHAR *GetWideString(const char *strA)
{
    if (strA)
    {
        WCHAR *strW;
        int len = MultiByteToWideChar(CP_ACP, 0, strA, -1, NULL, 0);

        strW = heap_xalloc(len * sizeof(WCHAR));
        MultiByteToWideChar(CP_ACP, 0, strA, -1, strW, len);
        return strW;
    }
    return NULL;
}

static WCHAR *(*get_line)(FILE *);

/* parser definitions */
enum parser_state
{
    HEADER,              /* parsing the registry file version header */
    PARSE_WIN31_LINE,    /* parsing a Windows 3.1 registry line */
    LINE_START,          /* at the beginning of a registry line */
    KEY_NAME,            /* parsing a key name */
    DEFAULT_VALUE_NAME,  /* parsing a default value name */
    QUOTED_VALUE_NAME,   /* parsing a double-quoted value name */
    DATA_START,          /* preparing for data parsing operations */
    DATA_TYPE,           /* parsing the registry data type */
    STRING_DATA,         /* parsing REG_SZ data */
    DWORD_DATA,          /* parsing DWORD data */
    SET_VALUE,           /* adding a value to the registry */
    NB_PARSER_STATES
};

struct parser
{
    FILE              *file;           /* pointer to a registry file */
    WCHAR              two_wchars[2];  /* first two characters from the encoding check */
    BOOL               is_unicode;     /* parsing Unicode or ASCII data */
    short int          reg_version;    /* registry file version */
    HKEY               hkey;           /* current registry key */
    WCHAR             *key_name;       /* current key name */
    WCHAR             *value_name;     /* value name */
    DWORD              parse_type;     /* generic data type for parsing */
    DWORD              data_type;      /* data type */
    void              *data;           /* value data */
    DWORD              data_size;      /* size of the data (in bytes) */
    enum parser_state  state;          /* current parser state */
};

typedef WCHAR *(*parser_state_func)(struct parser *parser, WCHAR *pos);

/* parser state machine functions */
static WCHAR *header_state(struct parser *parser, WCHAR *pos);
static WCHAR *parse_win31_line_state(struct parser *parser, WCHAR *pos);
static WCHAR *line_start_state(struct parser *parser, WCHAR *pos);
static WCHAR *key_name_state(struct parser *parser, WCHAR *pos);
static WCHAR *default_value_name_state(struct parser *parser, WCHAR *pos);
static WCHAR *quoted_value_name_state(struct parser *parser, WCHAR *pos);
static WCHAR *data_start_state(struct parser *parser, WCHAR *pos);
static WCHAR *data_type_state(struct parser *parser, WCHAR *pos);
static WCHAR *string_data_state(struct parser *parser, WCHAR *pos);
static WCHAR *dword_data_state(struct parser *parser, WCHAR *pos);
static WCHAR *set_value_state(struct parser *parser, WCHAR *pos);

static const parser_state_func parser_funcs[NB_PARSER_STATES] =
{
    header_state,              /* HEADER */
    parse_win31_line_state,    /* PARSE_WIN31_LINE */
    line_start_state,          /* LINE_START */
    key_name_state,            /* KEY_NAME */
    default_value_name_state,  /* DEFAULT_VALUE_NAME */
    quoted_value_name_state,   /* QUOTED_VALUE_NAME */
    data_start_state,          /* DATA_START */
    data_type_state,           /* DATA_TYPE */
    string_data_state,         /* STRING_DATA */
    dword_data_state,          /* DWORD_DATA */
    set_value_state,           /* SET_VALUE */
};

/* set the new parser state and return the previous one */
static inline enum parser_state set_state(struct parser *parser, enum parser_state state)
{
    enum parser_state ret = parser->state;
    parser->state = state;
    return ret;
}

/******************************************************************************
 * Converts a hex representation of a DWORD into a DWORD.
 */
static BOOL convert_hex_to_dword(WCHAR *str, DWORD *dw)
{
    WCHAR *p, *end;
    int count = 0;

    while (*str == ' ' || *str == '\t') str++;
    if (!*str) goto error;

    p = str;
    while (isxdigitW(*p))
    {
        count++;
        p++;
    }
    if (count > 8) goto error;

    end = p;
    while (*p == ' ' || *p == '\t') p++;
    if (*p && *p != ';') goto error;

    *end = 0;
    *dw = strtoulW(str, &end, 16);
    return TRUE;

error:
    return FALSE;
}

/******************************************************************************
 * Parses the data type of the registry value being imported and modifies
 * the input parameter to skip the string representation of the data type.
 *
 * Returns TRUE or FALSE to indicate whether a data type was found.
 */
static BOOL parse_data_type(struct parser *parser, WCHAR **line)
{
    struct data_type { const WCHAR *tag; int len; int type; int parse_type; };

    static const WCHAR quote[] = {'"'};
    static const WCHAR hex[] = {'h','e','x',':'};
    static const WCHAR dword[] = {'d','w','o','r','d',':'};
    static const WCHAR hexp[] = {'h','e','x','('};

    static const struct data_type data_types[] = {
    /*    tag    len  type         parse type    */
        { quote,  1,  REG_SZ,      REG_SZ },
        { hex,    4,  REG_BINARY,  REG_BINARY },
        { dword,  6,  REG_DWORD,   REG_DWORD },
        { hexp,   4,  -1,          REG_BINARY }, /* REG_NONE, REG_EXPAND_SZ, REG_MULTI_SZ */
        { NULL,   0,  0,           0 }
    };

    const struct data_type *ptr;

    for (ptr = data_types; ptr->tag; ptr++)
    {
        if (strncmpW(ptr->tag, *line, ptr->len))
            continue;

        parser->parse_type = ptr->parse_type;
        parser->data_type = ptr->parse_type;
        *line += ptr->len;

        if (ptr->type == -1)
        {
            WCHAR *end;
            DWORD val;

            if (!**line || tolowerW((*line)[1]) == 'x')
                return FALSE;

            /* "hex(xx):" is special */
            val = wcstoul(*line, &end, 16);
            if (*end != ')' || *(end + 1) != ':' || (val == ~0u && errno == ERANGE))
                return FALSE;

            parser->data_type = val;
            *line = end + 2;
        }
        return TRUE;
    }
    return FALSE;
}

/******************************************************************************
 * Replaces escape sequences with their character equivalents and
 * null-terminates the string on the first non-escaped double quote.
 *
 * Assigns a pointer to the remaining unparsed data in the line.
 * Returns TRUE or FALSE to indicate whether a closing double quote was found.
 */
static BOOL unescape_string(WCHAR *str, WCHAR **unparsed)
{
    int str_idx = 0;            /* current character under analysis */
    int val_idx = 0;            /* the last character of the unescaped string */
    int len = lstrlenW(str);
    BOOL ret;

    for (str_idx = 0; str_idx < len; str_idx++, val_idx++)
    {
        if (str[str_idx] == '\\')
        {
            str_idx++;
            switch (str[str_idx])
            {
            case 'n':
                str[val_idx] = '\n';
                break;
            case 'r':
                str[val_idx] = '\r';
                break;
            case '0':
                str[val_idx] = '\0';
                break;
            case '\\':
            case '"':
                str[val_idx] = str[str_idx];
                break;
            default:
                if (!str[str_idx]) return FALSE;
                output_message(STRING_ESCAPE_SEQUENCE, str[str_idx]);
                str[val_idx] = str[str_idx];
                break;
            }
        }
        else if (str[str_idx] == '"')
            break;
        else
            str[val_idx] = str[str_idx];
    }

    ret = (str[str_idx] == '"');
    *unparsed = str + str_idx + 1;
    str[val_idx] = '\0';
    return ret;
}

static HKEY parse_key_name(WCHAR *key_name, WCHAR **key_path)
{
    if (!key_name) return 0;

    *key_path = strchrW(key_name, '\\');
    if (*key_path) (*key_path)++;

    return path_get_rootkey(key_name);
}

static void close_key(struct parser *parser)
{
    if (parser->hkey)
    {
        heap_free(parser->key_name);
        parser->key_name = NULL;

        RegCloseKey(parser->hkey);
        parser->hkey = NULL;
    }
}

static LONG open_key(struct parser *parser, WCHAR *path)
{
    HKEY key_class;
    WCHAR *key_path;
    LONG res;

    close_key(parser);

    /* Get the registry class */
    if (!path || !(key_class = parse_key_name(path, &key_path)))
        return ERROR_INVALID_PARAMETER;

    res = RegCreateKeyExW(key_class, key_path, 0, NULL, REG_OPTION_NON_VOLATILE,
                          KEY_ALL_ACCESS, NULL, &parser->hkey, NULL);

    if (res == ERROR_SUCCESS)
    {
        parser->key_name = heap_xalloc((lstrlenW(path) + 1) * sizeof(WCHAR));
        lstrcpyW(parser->key_name, path);
    }
    else
        parser->hkey = NULL;

    return res;
}

static void free_parser_data(struct parser *parser)
{
    if (parser->parse_type == REG_DWORD)
        heap_free(parser->data);

    parser->data = NULL;
    parser->data_size = 0;
}

enum reg_versions {
    REG_VERSION_31,
    REG_VERSION_40,
    REG_VERSION_50,
    REG_VERSION_FUZZY,
    REG_VERSION_INVALID
};

static enum reg_versions parse_file_header(const WCHAR *s)
{
    static const WCHAR header_31[] = {'R','E','G','E','D','I','T',0};
    static const WCHAR header_40[] = {'R','E','G','E','D','I','T','4',0};
    static const WCHAR header_50[] = {'W','i','n','d','o','w','s',' ',
                                      'R','e','g','i','s','t','r','y',' ','E','d','i','t','o','r',' ',
                                      'V','e','r','s','i','o','n',' ','5','.','0','0',0};

    while (*s == ' ' || *s == '\t') s++;

    if (!strcmpW(s, header_31))
        return REG_VERSION_31;

    if (!strcmpW(s, header_40))
        return REG_VERSION_40;

    if (!strcmpW(s, header_50))
        return REG_VERSION_50;

    /* The Windows version accepts registry file headers beginning with "REGEDIT" and ending
     * with other characters, as long as "REGEDIT" appears at the start of the line. For example,
     * "REGEDIT 4", "REGEDIT9" and "REGEDIT4FOO" are all treated as valid file headers.
     * In all such cases, however, the contents of the registry file are not imported.
     */
    if (!strncmpW(s, header_31, 7)) /* "REGEDIT" without NUL */
        return REG_VERSION_FUZZY;

    return REG_VERSION_INVALID;
}

/* handler for parser HEADER state */
static WCHAR *header_state(struct parser *parser, WCHAR *pos)
{
    WCHAR *line, *header;

    if (!(line = get_line(parser->file)))
        return NULL;

    if (!parser->is_unicode)
    {
        header = heap_xalloc((lstrlenW(line) + 3) * sizeof(WCHAR));
        header[0] = parser->two_wchars[0];
        header[1] = parser->two_wchars[1];
        lstrcpyW(header + 2, line);
        parser->reg_version = parse_file_header(header);
        heap_free(header);
    }
    else parser->reg_version = parse_file_header(line);

    switch (parser->reg_version)
    {
    case REG_VERSION_31:
        set_state(parser, PARSE_WIN31_LINE);
        break;
    case REG_VERSION_40:
    case REG_VERSION_50:
        set_state(parser, LINE_START);
        break;
    default:
        get_line(NULL); /* Reset static variables */
        return NULL;
    }

    return line;
}

/* handler for parser PARSE_WIN31_LINE state */
static WCHAR *parse_win31_line_state(struct parser *parser, WCHAR *pos)
{
    WCHAR *line, *value;
    static WCHAR hkcr[] = {'H','K','E','Y','_','C','L','A','S','S','E','S','_','R','O','O','T'};
    unsigned int key_end = 0;

    if (!(line = get_line(parser->file)))
        return NULL;

    if (strncmpW(line, hkcr, ARRAY_SIZE(hkcr)))
        return line;

    /* get key name */
    while (line[key_end] && !isspaceW(line[key_end])) key_end++;

    value = line + key_end;
    while (*value == ' ' || *value == '\t') value++;

    if (*value == '=') value++;
    if (*value == ' ') value++; /* at most one space is skipped */

    line[key_end] = 0;

    if (open_key(parser, line) != ERROR_SUCCESS)
    {
        output_message(STRING_OPEN_KEY_FAILED, line);
        return line;
    }

    parser->value_name = NULL;
    parser->data_type = REG_SZ;
    parser->data = value;
    parser->data_size = (lstrlenW(value) + 1) * sizeof(WCHAR);

    set_state(parser, SET_VALUE);
    return value;
}

/* handler for parser LINE_START state */
static WCHAR *line_start_state(struct parser *parser, WCHAR *pos)
{
    WCHAR *line, *p;

    if (!(line = get_line(parser->file)))
        return NULL;

    for (p = line; *p; p++)
    {
        switch (*p)
        {
        case '[':
            set_state(parser, KEY_NAME);
            return p + 1;
        case '@':
            set_state(parser, DEFAULT_VALUE_NAME);
            return p;
        case '"':
            set_state(parser, QUOTED_VALUE_NAME);
            return p + 1;
        case ' ':
        case '\t':
            break;
        default:
            return p;
        }
    }

    return p;
}

/* handler for parser KEY_NAME state */
static WCHAR *key_name_state(struct parser *parser, WCHAR *pos)
{
    WCHAR *p = pos, *key_end;

    if (*p == ' ' || *p == '\t' || !(key_end = strrchrW(p, ']')))
        goto done;

    *key_end = 0;

    if (*p == '-')
    {
        FIXME("key deletion not yet implemented\n");
        goto done;
    }
    else if (open_key(parser, p) != ERROR_SUCCESS)
        output_message(STRING_OPEN_KEY_FAILED, p);

done:
    set_state(parser, LINE_START);
    return p;
}

/* handler for parser DEFAULT_VALUE_NAME state */
static WCHAR *default_value_name_state(struct parser *parser, WCHAR *pos)
{
    heap_free(parser->value_name);
    parser->value_name = NULL;

    set_state(parser, DATA_START);
    return pos + 1;
}

/* handler for parser QUOTED_VALUE_NAME state */
static WCHAR *quoted_value_name_state(struct parser *parser, WCHAR *pos)
{
    WCHAR *val_name = pos, *p;

    if (parser->value_name)
    {
        heap_free(parser->value_name);
        parser->value_name = NULL;
    }

    if (!unescape_string(val_name, &p))
        goto invalid;

    /* copy the value name in case we need to parse multiple lines and the buffer is overwritten */
    parser->value_name = heap_xalloc((lstrlenW(val_name) + 1) * sizeof(WCHAR));
    lstrcpyW(parser->value_name, val_name);

    set_state(parser, DATA_START);
    return p;

invalid:
    set_state(parser, LINE_START);
    return val_name;
}

/* handler for parser DATA_START state */
static WCHAR *data_start_state(struct parser *parser, WCHAR *pos)
{
    WCHAR *p = pos;
    unsigned int len;

    while (*p == ' ' || *p == '\t') p++;
    if (*p != '=') goto invalid;
    p++;
    while (*p == ' ' || *p == '\t') p++;

    /* trim trailing whitespace */
    len = strlenW(p);
    while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '\t')) len--;
    p[len] = 0;

    if (*p == '-')
    {
        FIXME("value deletion not yet implemented\n");
        goto invalid;
    }
    else
        set_state(parser, DATA_TYPE);
    return p;

invalid:
    set_state(parser, LINE_START);
    return p;
}

/* handler for parser DATA_TYPE state */
static WCHAR *data_type_state(struct parser *parser, WCHAR *pos)
{
    WCHAR *line = pos;

    if (!parse_data_type(parser, &line))
    {
        set_state(parser, LINE_START);
        return line;
    }

    switch (parser->parse_type)
    {
    case REG_SZ:
        set_state(parser, STRING_DATA);
        break;
    case REG_DWORD:
        set_state(parser, DWORD_DATA);
        break;
    case REG_BINARY: /* all hex data types, including undefined */
    default:
        set_state(parser, LINE_START);
    }

    return line;
}

/* handler for parser STRING_DATA state */
static WCHAR *string_data_state(struct parser *parser, WCHAR *pos)
{
    WCHAR *line;

    parser->data = pos;

    if (!unescape_string(parser->data, &line))
        goto invalid;

    while (*line == ' ' || *line == '\t') line++;
    if (*line && *line != ';') goto invalid;

    parser->data_size = (lstrlenW(parser->data) + 1) * sizeof(WCHAR);

    set_state(parser, SET_VALUE);
    return line;

invalid:
    free_parser_data(parser);
    set_state(parser, LINE_START);
    return line;
}

/* handler for parser DWORD_DATA state */
static WCHAR *dword_data_state(struct parser *parser, WCHAR *pos)
{
    WCHAR *line = pos;

    parser->data = heap_xalloc(sizeof(DWORD));

    if (!convert_hex_to_dword(line, parser->data))
        goto invalid;

    parser->data_size = sizeof(DWORD);

    set_state(parser, SET_VALUE);
    return line;

invalid:
    free_parser_data(parser);
    set_state(parser, LINE_START);
    return line;
}

/* handler for parser SET_VALUE state */
static WCHAR *set_value_state(struct parser *parser, WCHAR *pos)
{
    RegSetValueExW(parser->hkey, parser->value_name, 0, parser->data_type,
                   parser->data, parser->data_size);

    free_parser_data(parser);

    if (parser->reg_version == REG_VERSION_31)
        set_state(parser, PARSE_WIN31_LINE);
    else
        set_state(parser, LINE_START);

    return pos;
}

#define REG_VAL_BUF_SIZE  4096

static WCHAR *get_lineA(FILE *fp)
{
    static WCHAR *lineW;
    static size_t size;
    static char *buf, *next;
    char *line;

    heap_free(lineW);

    if (!fp) goto cleanup;

    if (!size)
    {
        size = REG_VAL_BUF_SIZE;
        buf = heap_xalloc(size);
        *buf = 0;
        next = buf;
    }
    line = next;

    while (next)
    {
        char *p = strpbrk(line, "\r\n");
        if (!p)
        {
            size_t len, count;
            len = strlen(next);
            memmove(buf, next, len + 1);
            if (size - len < 3)
            {
                size *= 2;
                buf = heap_xrealloc(buf, size);
            }
            if (!(count = fread(buf + len, 1, size - len - 1, fp)))
            {
                next = NULL;
                lineW = GetWideString(buf);
                return lineW;
            }
            buf[len + count] = 0;
            next = buf;
            line = buf;
            continue;
        }
        next = p + 1;
        if (*p == '\r' && *(p + 1) == '\n') next++;
        *p = 0;
        lineW = GetWideString(line);
        return lineW;
    }

cleanup:
    lineW = NULL;
    if (size) heap_free(buf);
    size = 0;
    return NULL;
}

static WCHAR *get_lineW(FILE *fp)
{
    static size_t size;
    static WCHAR *buf, *next;
    WCHAR *line;

    if (!fp) goto cleanup;

    if (!size)
    {
        size = REG_VAL_BUF_SIZE;
        buf = heap_xalloc(size * sizeof(WCHAR));
        *buf = 0;
        next = buf;
    }
    line = next;

    while (next)
    {
        static const WCHAR line_endings[] = {'\r','\n',0};
        WCHAR *p = strpbrkW(line, line_endings);
        if (!p)
        {
            size_t len, count;
            len = strlenW(next);
            memmove(buf, next, (len + 1) * sizeof(WCHAR));
            if (size - len < 3)
            {
                size *= 2;
                buf = heap_xrealloc(buf, size * sizeof(WCHAR));
            }
            if (!(count = fread(buf + len, sizeof(WCHAR), size - len - 1, fp)))
            {
                next = NULL;
                return buf;
            }
            buf[len + count] = 0;
            next = buf;
            line = buf;
            continue;
        }
        next = p + 1;
        if (*p == '\r' && *(p + 1) == '\n') next++;
        *p = 0;
        return line;
    }

cleanup:
    if (size) heap_free(buf);
    size = 0;
    return NULL;
}

int reg_import(const WCHAR *filename)
{
    FILE *fp;
    static const WCHAR rb_mode[] = {'r','b',0};
    BYTE s[2];
    struct parser parser;
    WCHAR *pos;

    fp = _wfopen(filename, rb_mode);
    if (!fp)
    {
        output_message(STRING_FILE_NOT_FOUND, filename);
        return 1;
    }

    if (fread(s, sizeof(WCHAR), 1, fp) != 1)
        goto error;

    parser.is_unicode = (s[0] == 0xff && s[1] == 0xfe);
    get_line = parser.is_unicode ? get_lineW : get_lineA;

    parser.file          = fp;
    parser.two_wchars[0] = s[0];
    parser.two_wchars[1] = s[1];
    parser.reg_version   = -1;
    parser.hkey          = NULL;
    parser.key_name      = NULL;
    parser.value_name    = NULL;
    parser.parse_type    = 0;
    parser.data_type     = 0;
    parser.data          = NULL;
    parser.data_size     = 0;
    parser.state         = HEADER;

    pos = parser.two_wchars;

    /* parser main loop */
    while (pos)
        pos = (parser_funcs[parser.state])(&parser, pos);

    if (parser.reg_version == REG_VERSION_INVALID)
        goto error;
    else if (parser.reg_version == REG_VERSION_40 || parser.reg_version == REG_VERSION_50)
    {
        FIXME(": operation not yet implemented\n");
        heap_free(parser.value_name);
        goto error;
    }

    heap_free(parser.value_name);
    close_key(&parser);

    fclose(fp);
    return 0;

error:
    fclose(fp);
    return 1;
}
