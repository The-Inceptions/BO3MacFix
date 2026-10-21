// printsubtitle's arguments, see t7o_subtitle.h
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "t7o_subtitle.h"

// argv[index] as a whole number from min to max, or fallback
static int number_arg(int argc, const char *const *argv, int index, int min, int max, int fallback) {
    if (index >= argc || argv[index] == NULL || argv[index][0] == '\0')
        return fallback;
    char *end = NULL;
    errno = 0;
    long value = strtol(argv[index], &end, 10);
    if (errno != 0 || *end != '\0' || value < min || value > max)
        return fallback;
    return (int)value;
}

bool t7o_parse_printsubtitle(int argc, const char *const *argv, int local_clients, t7o_subtitle *out) {
    if (out == NULL || argv == NULL || argc < 2 || argv[1] == NULL || argv[1][0] == '\0')
        return false;
    // room for the line feed and the terminator
    size_t length = strnlen(argv[1], sizeof(out->text) - 2);
    memcpy(out->text, argv[1], length);
    if (length == 0 || out->text[length - 1] != '\n')
        out->text[length++] = '\n';
    out->text[length] = '\0';
    out->channel = number_arg(argc, argv, 2, 0, T7o_Channel_Count - 1, T7o_Channel_GameNotify);
    out->duration = number_arg(argc, argv, 3, 0, INT_MAX, 0);
    out->local_client = local_clients > 0 ? number_arg(argc, argv, 4, 0, local_clients - 1, 0) : 0;
    out->pixel_width = number_arg(argc, argv, 5, 0, INT_MAX, 0);
    out->flags = number_arg(argc, argv, 6, 0, INT_MAX, 0);
    return true;
}

// a menu or response name that can go on a console line as it is
static bool plain_name(const char *name) {
    if (name == NULL || name[0] == '\0')
        return false;
    for (const unsigned char *c = (const unsigned char *)name; *c != '\0'; c++) {
        if (*c < 0x20 || *c == 0x7f || *c == ';' || *c == '"')
            return false;
    }
    return true;
}

bool t7o_parse_sendmenuresponse(int argc, const char *const *argv, t7o_menu_response *out) {
    if (out == NULL || argv == NULL || argc < 3 || !plain_name(argv[1]) || !plain_name(argv[2]))
        return false;
    out->menu = argv[1];
    out->response = argv[2];
    return true;
}

bool t7o_format_menu_response(char *buffer, size_t size, int server_id, int menu_index, int response_index,
                              const char *response) {
    if (buffer == NULL || size == 0)
        return false;
    int length;
    if (response_index > 0)
        length = snprintf(buffer, size, "cmd mrp %i %i %i\n", server_id, menu_index, response_index);
    else if (plain_name(response))
        length = snprintf(buffer, size, "cmd mr %i %i %s\n", server_id, menu_index, response);
    else
        length = -1;
    if (length < 0 || (size_t)length >= size) {
        buffer[0] = '\0';
        return false;
    }
    return true;
}
