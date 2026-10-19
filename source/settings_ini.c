// Reading and writing BO3MacFix's settings.ini, see settings_ini.h.
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "settings_ini.h"

// longer lines are read in pieces: only the first piece can set anything, and the writer keeps them whole
#define Line_Size 1024

const char *settings_ini_match(const char *line, const char *name) {
    // the UTF-8 byte order mark some editors put at the start of a file
    if (strncmp(line, "\xEF\xBB\xBF", 3) == 0)
        line += 3;
    const char *p = line + strspn(line, " \t");
    size_t name_len = strlen(name);
    if (name_len == 0 || strncmp(p, name, name_len) != 0)
        return NULL;
    p += name_len;
    p += strspn(p, " \t");
    return *p == '=' ? p + 1 : NULL;
}

bool settings_ini_number(const char *value, int base, int *out) {
    const char *p = value + strspn(value, " \t\"'");
    if (base == 16 && *p == '#')
        p++;
    char *end;
    long number = strtol(p, &end, base);
    // nothing there (Name =) or not a number: not 0, which would turn the setting off
    if (end == p)
        return false;
    *out = number > INT_MAX ? INT_MAX : number < INT_MIN ? INT_MIN : (int)number;
    return true;
}

int settings_ini_read(const char *path, const char *name, int fallback, int base) {
    FILE *fp = fopen(path, "r");
    if (fp == NULL)
        return fallback;
    int value = fallback;
    char line[Line_Size];
    bool line_start = true;
    while (fgets(line, sizeof(line), fp) != NULL) {
        const char *at = line_start ? settings_ini_match(line, name) : NULL;
        int number;
        if (at != NULL && settings_ini_number(at, base, &number))
            value = number;
        size_t len = strlen(line);
        line_start = len > 0 && line[len - 1] == '\n';
    }
    fclose(fp);
    return value;
}

bool settings_ini_read_string(const char *path, const char *name, char *out, size_t out_size) {
    FILE *fp = fopen(path, "r");
    if (fp == NULL)
        return false;
    bool found = false;
    char line[Line_Size];
    bool line_start = true;
    while (fgets(line, sizeof(line), fp) != NULL) {
        size_t len = strlen(line);
        bool line_end = len > 0 && line[len - 1] == '\n';
        const char *at = line_start ? settings_ini_match(line, name) : NULL;
        // a line too long to read whole doesn't set it
        if (at != NULL && (line_end || feof(fp))) {
            at += strspn(at, " \t");
            size_t n = strcspn(at, "\r\n");
            while (n > 0 && (at[n - 1] == ' ' || at[n - 1] == '\t'))
                n--;
            if (n >= 2 && (at[0] == '"' || at[0] == '\'') && at[n - 1] == at[0]) {
                at++;
                n -= 2;
            }
            if (n > 0 && n < out_size) {
                memcpy(out, at, n);
                out[n] = '\0';
                found = true;
            }
        }
        line_start = line_end;
    }
    fclose(fp);
    return found;
}

bool settings_ini_write(const char *path, const char *name, const char *value) {
    char temp[PATH_MAX + 32];
    if (snprintf(temp, sizeof(temp), "%s.tmp%d", path, (int)getpid()) >= (int)sizeof(temp))
        return false;
    FILE *in = fopen(path, "r");
    // a settings.ini that's there but can't be read mustn't be replaced by one with only this line
    if (in == NULL && errno != ENOENT)
        return false;
    FILE *out = fopen(temp, "w");
    if (out == NULL) {
        if (in != NULL)
            fclose(in);
        return false;
    }
    const char *eol = "\n"; // \r\n if the file uses that
    bool written = false, line_start = true, dropping = false, ends_with_newline = true;
    char line[Line_Size];
    while (in != NULL && fgets(line, sizeof(line), in) != NULL) {
        size_t len = strlen(line);
        bool line_end = len > 0 && line[len - 1] == '\n';
        if (line_end && len >= 2 && line[len - 2] == '\r')
            eol = "\r\n";
        if (line_start)
            dropping = settings_ini_match(line, name) != NULL;
        if (!dropping) {
            fputs(line, out);
        } else if (!written && (line_end || feof(in))) {
            // the first line that sets it (all of it, however long) becomes the new line, any later ones go
            fprintf(out, "%s = %s%s", name, value, eol);
            written = true;
        }
        line_start = line_end;
        ends_with_newline = line_end;
    }
    // a read or write error part way mustn't replace settings.ini with what we have so far
    bool failed = in != NULL && ferror(in);
    if (in != NULL)
        fclose(in);
    if (!written)
        fprintf(out, "%s%s = %s%s", ends_with_newline ? "" : eol, name, value, eol);
    failed = failed || ferror(out) || fflush(out) != 0 || fsync(fileno(out)) != 0;
    failed = fclose(out) != 0 || failed;
    if (failed || rename(temp, path) != 0) {
        unlink(temp);
        return false;
    }
    return true;
}
