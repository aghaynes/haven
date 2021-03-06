
#include <stdlib.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h>

#include "readstat.h"
#include "readstat_dta.h"
#include "readstat_writer.h"

#define DTA_DEFAULT_FORMAT_BYTE    "8.0g"
#define DTA_DEFAULT_FORMAT_INT16   "8.0g"
#define DTA_DEFAULT_FORMAT_INT32  "12.0g"
#define DTA_DEFAULT_FORMAT_FLOAT   "9.0g"
#define DTA_DEFAULT_FORMAT_DOUBLE "10.0g"

#define DTA_DEFAULT_FILE_VERSION   118

#define DTA_OLD_MAX_WIDTH    128
#define DTA_111_MAX_WIDTH    244
#define DTA_117_MAX_WIDTH   2045

static readstat_error_t dta_113_write_missing_numeric(void *row, const readstat_variable_t *var);

static readstat_error_t dta_write_tag(readstat_writer_t *writer, dta_ctx_t *ctx, const char *tag) {
    if (!ctx->file_is_xmlish)
        return READSTAT_OK;

    return readstat_write_string(writer, tag);
}

static readstat_error_t dta_write_chunk(readstat_writer_t *writer, dta_ctx_t *ctx, 
        const char *start_tag,
        const void *bytes, size_t len,
        const char *end_tag) {
    readstat_error_t error = READSTAT_OK;

    if ((error = dta_write_tag(writer, ctx, start_tag)) != READSTAT_OK)
        goto cleanup;

    if ((error = readstat_write_bytes(writer, bytes, len)) != READSTAT_OK)
        goto cleanup;

    if ((error = dta_write_tag(writer, ctx, end_tag)) != READSTAT_OK)
        goto cleanup;

cleanup:
    return error;
}


static readstat_error_t dta_emit_header_data_label(readstat_writer_t *writer, dta_ctx_t *ctx) {
    readstat_error_t error = READSTAT_OK;
    char *data_label = NULL;
    if ((error = dta_write_tag(writer, ctx, "<label>")) != READSTAT_OK)
        goto cleanup;

    if (ctx->data_label_len_len) {
        if (ctx->data_label_len_len == 1) {
            uint8_t len  = strlen(writer->file_label);
            if ((error = readstat_write_bytes(writer, &len, sizeof(uint8_t))) != READSTAT_OK)
                goto cleanup;
        } else if (ctx->data_label_len_len == 2) {
            uint16_t len  = strlen(writer->file_label);
            if ((error = readstat_write_bytes(writer, &len, sizeof(uint16_t))) != READSTAT_OK)
                goto cleanup;
        }
        if ((error = readstat_write_string(writer, writer->file_label)) != READSTAT_OK)
            goto cleanup;
    } else {
        data_label = calloc(1, ctx->data_label_len);
        snprintf(data_label, ctx->data_label_len, "%s", writer->file_label);

        if ((error = readstat_write_bytes(writer, data_label, ctx->data_label_len)) != READSTAT_OK)
            goto cleanup;
    }

    if ((error = dta_write_tag(writer, ctx, "</label>")) != READSTAT_OK)
        goto cleanup;

cleanup:
    if (data_label)
        free(data_label);

    return error;
}

static readstat_error_t dta_emit_header_time_stamp(readstat_writer_t *writer, dta_ctx_t *ctx) {
    if (!ctx->time_stamp_len)
        return READSTAT_OK;

    readstat_error_t error = READSTAT_OK;
    time_t now = time(NULL);
    struct tm *time_s = localtime(&now);
    char *time_stamp = calloc(1, ctx->time_stamp_len);
    uint8_t actual_time_stamp_len = strftime(time_stamp, ctx->time_stamp_len, "%d %b %Y %H:%M", time_s);
    if (actual_time_stamp_len == 0) {
        error = READSTAT_ERROR_WRITE;
        goto cleanup;
    }

    if (ctx->file_is_xmlish) {
        if ((error = dta_write_tag(writer, ctx, "<timestamp>")) != READSTAT_OK)
            goto cleanup;

        if ((error = readstat_write_bytes(writer, &actual_time_stamp_len, sizeof(uint8_t))) != READSTAT_OK)
            goto cleanup;

        if ((error = readstat_write_bytes(writer, time_stamp, actual_time_stamp_len)) != READSTAT_OK)
            goto cleanup;

        if ((error = dta_write_tag(writer, ctx, "</timestamp>")) != READSTAT_OK)
            goto cleanup;
    } else {
        error = readstat_write_bytes(writer, time_stamp, ctx->time_stamp_len);
    }

cleanup:
    free(time_stamp);
    return error;
}

static uint16_t dta_111_typecode_for_variable(readstat_variable_t *r_variable) {
    size_t max_len = r_variable->storage_width;
    uint16_t typecode = 0;
    switch (r_variable->type) {
        case READSTAT_TYPE_CHAR: 
            typecode = DTA_111_TYPE_CODE_CHAR; break;
        case READSTAT_TYPE_INT16:
            typecode = DTA_111_TYPE_CODE_INT16; break;
        case READSTAT_TYPE_INT32:
            typecode = DTA_111_TYPE_CODE_INT32; break;
        case READSTAT_TYPE_FLOAT:
            typecode = DTA_111_TYPE_CODE_FLOAT; break;
        case READSTAT_TYPE_DOUBLE:
            typecode = DTA_111_TYPE_CODE_DOUBLE; break;
        case READSTAT_TYPE_STRING:
        case READSTAT_TYPE_LONG_STRING:
            if (max_len > DTA_111_MAX_WIDTH)
                max_len = DTA_111_MAX_WIDTH;

            typecode = max_len;
            break;
    }
    return typecode;
}

static uint16_t dta_117_typecode_for_variable(readstat_variable_t *r_variable) {
    size_t max_len = r_variable->storage_width;
    uint16_t typecode = 0;
    switch (r_variable->type) {
        case READSTAT_TYPE_CHAR: 
            typecode = DTA_117_TYPE_CODE_CHAR; break;
        case READSTAT_TYPE_INT16:
            typecode = DTA_117_TYPE_CODE_INT16; break;
        case READSTAT_TYPE_INT32:
            typecode = DTA_117_TYPE_CODE_INT32; break;
        case READSTAT_TYPE_FLOAT:
            typecode = DTA_117_TYPE_CODE_FLOAT; break;
        case READSTAT_TYPE_DOUBLE:
            typecode = DTA_117_TYPE_CODE_DOUBLE; break;
        case READSTAT_TYPE_STRING:
        case READSTAT_TYPE_LONG_STRING:
            if (max_len > DTA_117_MAX_WIDTH)
                max_len = DTA_117_MAX_WIDTH;

            typecode = max_len;
            break;
    }
    return typecode;
}

static uint16_t dta_old_typecode_for_variable(readstat_variable_t *r_variable) {
    size_t max_len = r_variable->storage_width;
    uint16_t typecode = 0;
    switch (r_variable->type) {
        case READSTAT_TYPE_CHAR: 
            typecode = DTA_OLD_TYPE_CODE_CHAR; break;
        case READSTAT_TYPE_INT16:
            typecode = DTA_OLD_TYPE_CODE_INT16; break;
        case READSTAT_TYPE_INT32:
            typecode = DTA_OLD_TYPE_CODE_INT32; break;
        case READSTAT_TYPE_FLOAT:
            typecode = DTA_OLD_TYPE_CODE_FLOAT; break;
        case READSTAT_TYPE_DOUBLE:
            typecode = DTA_OLD_TYPE_CODE_DOUBLE; break;
        case READSTAT_TYPE_STRING:
        case READSTAT_TYPE_LONG_STRING:
            if (max_len > DTA_OLD_MAX_WIDTH)
                max_len = DTA_OLD_MAX_WIDTH;

            typecode = max_len + 0x7F;
            break;
    }
    return typecode;
}

static uint16_t dta_typecode_for_variable(readstat_variable_t *r_variable, int typlist_version) {
    if (typlist_version == 111) {
        return dta_111_typecode_for_variable(r_variable);
    }
    if (typlist_version == 117) {
        return dta_117_typecode_for_variable(r_variable);
    } 
    return dta_old_typecode_for_variable(r_variable);
}

static readstat_error_t dta_emit_typlist(readstat_writer_t *writer, dta_ctx_t *ctx) {
    readstat_error_t error = READSTAT_OK;
    int i;

    if ((error = dta_write_tag(writer, ctx, "<variable_types>")) != READSTAT_OK)
        goto cleanup;

    for (i=0; i<ctx->nvar; i++) {
        readstat_variable_t *r_variable = readstat_get_variable(writer, i);
        ctx->typlist[i] = dta_typecode_for_variable(r_variable, ctx->typlist_version);
    }

    for (i=0; i<ctx->nvar; i++) {
        if (ctx->typlist_entry_len == 1) {
            uint8_t byte = ctx->typlist[i];
            error = readstat_write_bytes(writer, &byte, sizeof(uint8_t));
        } else if (ctx->typlist_entry_len == 2) {
            uint16_t val = ctx->typlist[i];
            error = readstat_write_bytes(writer, &val, sizeof(uint16_t));
        }
        if (error != READSTAT_OK)
            goto cleanup;
    }

    if ((error = dta_write_tag(writer, ctx, "</variable_types>")) != READSTAT_OK)
        goto cleanup;

cleanup:
    return error;
}

static readstat_error_t dta_validate_name(const char *name) {
    char first_char = name[0];
    if (first_char != '_' &&
            !(first_char >= 'a' && first_char <= 'z') &&
            !(first_char >= 'A' && first_char <= 'Z')) {
        return READSTAT_ERROR_NAME_BEGINS_WITH_ILLEGAL_CHARACTER;
    }
    int j;
    for (j=1; name[j]; j++) {
        if (name[j] != '_' &&
                !(name[j] >= 'a' && name[j] <= 'z') &&
                !(name[j] >= 'A' && name[j] <= 'Z') &&
                !(name[j] >= '0' && name[j] <= '9')) {
            return READSTAT_ERROR_NAME_CONTAINS_ILLEGAL_CHARACTER;
        }
    }
    if (strcmp(name, "_all") == 0 || strcmp(name, "_b") == 0 ||
            strcmp(name, "byte") == 0 || strcmp(name, "_coef") == 0 ||
            strcmp(name, "_cons") == 0 || strcmp(name, "double") == 0 ||
            strcmp(name, "float") == 0 || strcmp(name, "if") == 0 ||
            strcmp(name, "in") == 0 || strcmp(name, "int") == 0 ||
            strcmp(name, "long") == 0 || strcmp(name, "_n") == 0 ||
            strcmp(name, "_N") == 0 || strcmp(name, "_pi") == 0 ||
            strcmp(name, "_pred") == 0 || strcmp(name, "_rc") == 0 ||
            strcmp(name, "_skip") == 0 || strcmp(name, "strL") == 0 ||
            strcmp(name, "using") == 0 || strcmp(name, "with") == 0) {
        return READSTAT_ERROR_NAME_IS_RESERVED_WORD;
    }
    int len;
    if (sscanf(name, "str%d", &len) == 1)
        return READSTAT_ERROR_NAME_IS_RESERVED_WORD;

    return READSTAT_OK;
}

static readstat_error_t dta_emit_varlist(readstat_writer_t *writer, dta_ctx_t *ctx) {
    readstat_error_t error = READSTAT_OK;
    int i;

    if ((error = dta_write_tag(writer, ctx, "<varnames>")) != READSTAT_OK)
        goto cleanup;

    for (i=0; i<ctx->nvar; i++) {
        readstat_variable_t *r_variable = readstat_get_variable(writer, i);

        error = dta_validate_name(r_variable->name);
        if (error != READSTAT_OK)
            goto cleanup;

        strncpy(&ctx->varlist[ctx->variable_name_len*i], 
                r_variable->name, ctx->variable_name_len);
    }

    if ((error = readstat_write_bytes(writer, ctx->varlist, ctx->varlist_len)) != READSTAT_OK)
        goto cleanup;

    if ((error = dta_write_tag(writer, ctx, "</varnames>")) != READSTAT_OK)
        goto cleanup;

cleanup:
    return error;
}

static readstat_error_t dta_emit_srtlist(readstat_writer_t *writer, dta_ctx_t *ctx) {
    readstat_error_t error = READSTAT_OK;

    if ((error = dta_write_tag(writer, ctx, "<sortlist>")) != READSTAT_OK)
        goto cleanup;

    memset(ctx->srtlist, '\0', ctx->srtlist_len);

    if ((error = readstat_write_bytes(writer, ctx->srtlist, ctx->srtlist_len)) != READSTAT_OK)
        goto cleanup;

    if ((error = dta_write_tag(writer, ctx, "</sortlist>")) != READSTAT_OK)
        goto cleanup;

cleanup:
    return error;
}

static readstat_error_t dta_emit_fmtlist(readstat_writer_t *writer, dta_ctx_t *ctx) {
    readstat_error_t error = READSTAT_OK;
    int i;

    if ((error = dta_write_tag(writer, ctx, "<formats>")) != READSTAT_OK)
        goto cleanup;

    for (i=0; i<ctx->nvar; i++) {
        readstat_variable_t *r_variable = readstat_get_variable(writer, i);

        if (r_variable->format[0]) {
            strncpy(&ctx->fmtlist[ctx->fmtlist_entry_len*i],
                    r_variable->format, ctx->fmtlist_entry_len);
        } else {
            char *format_spec = "9s";
            if (r_variable->type == READSTAT_TYPE_CHAR) {
                format_spec = DTA_DEFAULT_FORMAT_BYTE;
            } else if (r_variable->type == READSTAT_TYPE_INT16) {
                format_spec = DTA_DEFAULT_FORMAT_INT16;
            } else if (r_variable->type == READSTAT_TYPE_INT32) {
                format_spec = DTA_DEFAULT_FORMAT_INT32;
            } else if (r_variable->type == READSTAT_TYPE_FLOAT) {
                format_spec = DTA_DEFAULT_FORMAT_FLOAT;
            } else if (r_variable->type == READSTAT_TYPE_DOUBLE) {
                format_spec = DTA_DEFAULT_FORMAT_DOUBLE;
            }
            char format[64];
            sprintf(format, "%%%s%s", 
                    r_variable->alignment == READSTAT_ALIGNMENT_LEFT ? "-" : "",
                    format_spec);
            strncpy(&ctx->fmtlist[ctx->fmtlist_entry_len*i],
                    format, ctx->fmtlist_entry_len);
        }
    }

    if ((error = readstat_write_bytes(writer, ctx->fmtlist, ctx->fmtlist_len)) != READSTAT_OK)
        goto cleanup;

    if ((error = dta_write_tag(writer, ctx, "</formats>")) != READSTAT_OK)
        goto cleanup;

cleanup:
    return error;
}

static readstat_error_t dta_emit_lbllist(readstat_writer_t *writer, dta_ctx_t *ctx) {
    readstat_error_t error = READSTAT_OK;
    int i;

    if ((error = dta_write_tag(writer, ctx, "<value_label_names>")) != READSTAT_OK)
        goto cleanup;

    for (i=0; i<ctx->nvar; i++) {
        readstat_variable_t *r_variable = readstat_get_variable(writer, i);

        if (r_variable->label_set) {
            strncpy(&ctx->lbllist[ctx->lbllist_entry_len*i], 
                    r_variable->label_set->name, ctx->lbllist_entry_len);
        } else {
            memset(&ctx->lbllist[ctx->lbllist_entry_len*i], '\0', ctx->lbllist_entry_len);
        }
    }
    if ((error = readstat_write_bytes(writer, ctx->lbllist, ctx->lbllist_len)) != READSTAT_OK)
        goto cleanup;

    if ((error = dta_write_tag(writer, ctx, "</value_label_names>")) != READSTAT_OK)
        goto cleanup;

cleanup:
    return error;
}

static readstat_error_t dta_emit_descriptors(readstat_writer_t *writer, dta_ctx_t *ctx) {
    readstat_error_t error = READSTAT_OK;

    error = dta_emit_typlist(writer, ctx);
    if (error != READSTAT_OK)
        goto cleanup;

    error = dta_emit_varlist(writer, ctx);
    if (error != READSTAT_OK)
        goto cleanup;
    
    error = dta_emit_srtlist(writer, ctx);
    if (error != READSTAT_OK)
        goto cleanup;
    
    error = dta_emit_fmtlist(writer, ctx);
    if (error != READSTAT_OK)
        goto cleanup;
    
    error = dta_emit_lbllist(writer, ctx);
    if (error != READSTAT_OK)
        goto cleanup;

cleanup:
    return error;
}

static readstat_error_t dta_emit_variable_labels(readstat_writer_t *writer, dta_ctx_t *ctx) {
    readstat_error_t error = READSTAT_OK;
    int i;

    if ((error = dta_write_tag(writer, ctx, "<variable_labels>")) != READSTAT_OK)
        goto cleanup;

    for (i=0; i<ctx->nvar; i++) {
        readstat_variable_t *r_variable = readstat_get_variable(writer, i);

        strncpy(&ctx->variable_labels[ctx->variable_labels_entry_len*i], 
                r_variable->label, ctx->variable_labels_entry_len);
    }
    if ((error = readstat_write_bytes(writer, ctx->variable_labels, ctx->variable_labels_len)) != READSTAT_OK)
        goto cleanup;

    if ((error = dta_write_tag(writer, ctx, "</variable_labels>")) != READSTAT_OK)
        goto cleanup;

cleanup:
    return error;
}

static readstat_error_t dta_emit_characteristics(readstat_writer_t *writer, dta_ctx_t *ctx) {
    if (ctx->file_is_xmlish) {
        return readstat_write_string(writer, "<characteristics></characteristics>");
    }
    if (ctx->expansion_len_len == 2) {
        dta_short_expansion_field_t expansion_field = { .data_type = 0, .len = 0 };
        return readstat_write_bytes(writer, &expansion_field, sizeof(dta_short_expansion_field_t));
    } else if (ctx->expansion_len_len == 4) {
        dta_expansion_field_t expansion_field = { .data_type = 0, .len = 0 };
        return readstat_write_bytes(writer, &expansion_field, sizeof(dta_expansion_field_t));
    }
    return READSTAT_OK;
}

static readstat_error_t dta_emit_strls(readstat_writer_t *writer, dta_ctx_t *ctx) {
    if (!ctx->file_is_xmlish)
        return READSTAT_OK;

    return readstat_write_string(writer, "<strls></strls>");
}

static readstat_error_t dta_old_emit_value_labels(readstat_writer_t *writer, dta_ctx_t *ctx) {
    readstat_error_t retval = READSTAT_OK;
    int i, j;
    char labname[12+2];
    for (i=0; i<writer->label_sets_count; i++) {
        readstat_label_set_t *r_label_set = readstat_get_label_set(writer, i);
        int32_t n = r_label_set->value_labels_count;
        int16_t table_len = 8*n;
        retval = readstat_write_bytes(writer, &table_len, sizeof(int16_t));
        if (retval != READSTAT_OK)
            goto cleanup;

        memset(labname, 0, sizeof(labname));
        strncpy(labname, r_label_set->name, ctx->value_label_table_labname_len);

        retval = readstat_write_bytes(writer, labname, ctx->value_label_table_labname_len 
                + ctx->value_label_table_padding_len);
        if (retval != READSTAT_OK)
            goto cleanup;

        for (j=0; j<n; j++) {
            readstat_value_label_t *value_label = readstat_get_value_label(r_label_set, j);
            char label_buf[8];
            strncpy(label_buf, value_label->label, sizeof(label_buf));
            retval = readstat_write_bytes(writer, label_buf, sizeof(label_buf));
            if (retval != READSTAT_OK)
                goto cleanup;
        }
    }
cleanup:
    return retval;
}

static readstat_error_t dta_emit_value_labels(readstat_writer_t *writer, dta_ctx_t *ctx) {
    if (ctx->value_label_table_len_len == 2)
        return dta_old_emit_value_labels(writer, ctx);

    readstat_error_t retval = READSTAT_OK;
    int i, j;
    int32_t *off = NULL;
    int32_t *val = NULL;
    char *txt = NULL;
    char *labname = calloc(1, ctx->value_label_table_labname_len + ctx->value_label_table_padding_len);

    retval = dta_write_tag(writer, ctx, "<value_labels>");
    if (retval != READSTAT_OK)
        goto cleanup;

    for (i=0; i<writer->label_sets_count; i++) {
        readstat_label_set_t *r_label_set = readstat_get_label_set(writer, i);
        int32_t n = r_label_set->value_labels_count;
        int32_t txtlen = 0;
        for (j=0; j<n; j++) {
            readstat_value_label_t *value_label = readstat_get_value_label(r_label_set, j);
            txtlen += value_label->label_len + 1;
        }

        retval = dta_write_tag(writer, ctx, "<lbl>");
        if (retval != READSTAT_OK)
            goto cleanup;

        int32_t table_len = 8 + 8*n + txtlen;
        retval = readstat_write_bytes(writer, &table_len, sizeof(int32_t));
        if (retval != READSTAT_OK)
            goto cleanup;

        strncpy(labname, r_label_set->name, ctx->value_label_table_labname_len);

        retval = readstat_write_bytes(writer, labname, ctx->value_label_table_labname_len 
                + ctx->value_label_table_padding_len);
        if (retval != READSTAT_OK)
            goto cleanup;

        if (txtlen == 0) {
            retval = readstat_write_bytes(writer, &txtlen, sizeof(int32_t));
            if (retval != READSTAT_OK)
                goto cleanup;

            retval = readstat_write_bytes(writer, &txtlen, sizeof(int32_t));
            if (retval != READSTAT_OK)
                goto cleanup;

            retval = dta_write_tag(writer, ctx, "</lbl>");
            if (retval != READSTAT_OK)
                goto cleanup;

            continue;
        }

        off = realloc(off, 4*n);
        val = realloc(val, 4*n);
        txt = realloc(txt, txtlen);

        off_t offset = 0;

        for (j=0; j<n; j++) {
            readstat_value_label_t *value_label = readstat_get_value_label(r_label_set, j);
            const char *label = value_label->label;
            size_t label_data_len = value_label->label_len;
            off[j] = offset;
            val[j] = value_label->int32_key;
            memcpy(txt + offset, label, label_data_len);
            offset += label_data_len;

            txt[offset++] = '\0';                
        }

        retval = readstat_write_bytes(writer, &n, sizeof(int32_t));
        if (retval != READSTAT_OK)
            goto cleanup;

        retval = readstat_write_bytes(writer, &txtlen, sizeof(int32_t));
        if (retval != READSTAT_OK)
            goto cleanup;

        retval = readstat_write_bytes(writer, off, 4*n);
        if (retval != READSTAT_OK)
            goto cleanup;

        retval = readstat_write_bytes(writer, val, 4*n);
        if (retval != READSTAT_OK)
            goto cleanup;

        retval = readstat_write_bytes(writer, txt, txtlen);
        if (retval != READSTAT_OK)
            goto cleanup;

        retval = dta_write_tag(writer, ctx, "</lbl>");
        if (retval != READSTAT_OK)
            goto cleanup;
    }

    retval = dta_write_tag(writer, ctx, "</value_labels>");
    if (retval != READSTAT_OK)
        goto cleanup;

cleanup:
    if (off)
        free(off);
    if (val)
        free(val);
    if (txt)
        free(txt);
    if (labname)
        free(labname);

    return retval;
}

static size_t dta_numeric_variable_width(readstat_types_t type, size_t user_width) {
    size_t len = 0;
    if (type == READSTAT_TYPE_DOUBLE) {
        len = 8;
    } else if (type == READSTAT_TYPE_FLOAT) {
        len = 4;
    } else if (type == READSTAT_TYPE_INT32) {
        len = 4;
    } else if (type == READSTAT_TYPE_INT16) {
        len = 2;
    } else if (type == READSTAT_TYPE_CHAR) {
        len = 1;
    }
    return len;
}

static size_t dta_111_variable_width(readstat_types_t type, size_t user_width) {
    if (type == READSTAT_TYPE_STRING) {
        if (user_width > DTA_111_MAX_WIDTH || user_width == 0)
            user_width = DTA_111_MAX_WIDTH;
        return user_width;
    }
    return dta_numeric_variable_width(type, user_width);
}

static size_t dta_117_variable_width(readstat_types_t type, size_t user_width) {
    if (type == READSTAT_TYPE_STRING) {
        if (user_width > DTA_117_MAX_WIDTH || user_width == 0)
            user_width = DTA_117_MAX_WIDTH;
        return user_width;
    }
    return dta_numeric_variable_width(type, user_width);
}

static size_t dta_old_variable_width(readstat_types_t type, size_t user_width) {
    if (type == READSTAT_TYPE_STRING) {
        if (user_width > DTA_OLD_MAX_WIDTH || user_width == 0)
            user_width = DTA_OLD_MAX_WIDTH;
        return user_width;
    }
    return dta_numeric_variable_width(type, user_width);
}

static readstat_error_t dta_emit_header(readstat_writer_t *writer, dta_ctx_t *ctx,
        dta_header_t *header) {
    readstat_error_t error = READSTAT_OK;

    if (!ctx->file_is_xmlish) {
        error = readstat_write_bytes(writer, header, sizeof(dta_header_t));
        if (error != READSTAT_OK)
            goto cleanup;

        error = dta_emit_header_data_label(writer, ctx);
        if (error != READSTAT_OK)
            goto cleanup;

        error = dta_emit_header_time_stamp(writer, ctx);
        if (error != READSTAT_OK)
            goto cleanup;

        return READSTAT_OK;
    }

    if ((error = dta_write_tag(writer, ctx, "<stata_dta>")) != READSTAT_OK)
        goto cleanup;

    if ((error = dta_write_tag(writer, ctx, "<header>")) != READSTAT_OK)
        goto cleanup;

    char release[128];
    snprintf(release, sizeof(release), "<release>%d</release>", header->ds_format);
    if ((error = readstat_write_string(writer, release)) != READSTAT_OK)
        goto cleanup;

    error = dta_write_chunk(writer, ctx, "<byteorder>",
            (header->byteorder == DTA_HILO) ? "MSF" : "LSF", sizeof("MSF")-1,
            "</byteorder>");
    if (error != READSTAT_OK)
        goto cleanup;

    error = dta_write_chunk(writer, ctx, "<K>", &header->nvar, sizeof(int16_t), "</K>");
    if (error != READSTAT_OK)
        goto cleanup;

    if (header->ds_format >= 118) {
        int64_t nobs = header->nobs;
        error = dta_write_chunk(writer, ctx, "<N>", &nobs, sizeof(int64_t), "</N>");
        if (error != READSTAT_OK)
            goto cleanup;
    } else {
        error = dta_write_chunk(writer, ctx, "<N>", &header->nobs, sizeof(int32_t), "</N>");
        if (error != READSTAT_OK)
            goto cleanup;
    }

    error = dta_emit_header_data_label(writer, ctx);
    if (error != READSTAT_OK)
        goto cleanup;

    error = dta_emit_header_time_stamp(writer, ctx);
    if (error != READSTAT_OK)
        goto cleanup;

    if ((error = dta_write_tag(writer, ctx, "</header>")) != READSTAT_OK)
        goto cleanup;

cleanup:
    return error;
}

static size_t dta_measure_tag(dta_ctx_t *ctx, const char *tag) {
    if (!ctx->file_is_xmlish)
        return 0;

    return strlen(tag);
}

static size_t dta_measure_map(dta_ctx_t *ctx) {
    return (dta_measure_tag(ctx, "<map>") 
            + 14 * sizeof(uint64_t) 
            + dta_measure_tag(ctx, "</map>"));
}

static size_t dta_measure_typlist(dta_ctx_t *ctx) {
    return (dta_measure_tag(ctx, "<variable_types>") 
            + ctx->typlist_entry_len * ctx->nvar
            + dta_measure_tag(ctx, "</variable_types>"));
}

static size_t dta_measure_varlist(dta_ctx_t *ctx) {
    return (dta_measure_tag(ctx, "<varnames>") 
            + ctx->varlist_len
            + dta_measure_tag(ctx, "</varnames>"));
}

static size_t dta_measure_srtlist(dta_ctx_t *ctx) {
    return (dta_measure_tag(ctx, "<sortlist>")
            + ctx->srtlist_len
            + dta_measure_tag(ctx, "</sortlist>"));
}

static size_t dta_measure_fmtlist(dta_ctx_t *ctx) {
    return (dta_measure_tag(ctx, "<formats>")
            + ctx->fmtlist_len
            + dta_measure_tag(ctx, "</formats>"));
}

static size_t dta_measure_lbllist(dta_ctx_t *ctx) {
    return (dta_measure_tag(ctx, "<value_label_names>")
            + ctx->lbllist_len
            + dta_measure_tag(ctx, "</value_label_names>"));
}

static size_t dta_measure_variable_labels(dta_ctx_t *ctx) {
    return (dta_measure_tag(ctx, "<variable_labels>")
            + ctx->variable_labels_len
            + dta_measure_tag(ctx, "</variable_labels>"));
}

static size_t dta_measure_characteristics(dta_ctx_t *ctx) {
    return (dta_measure_tag(ctx, "<characteristics>")
            + dta_measure_tag(ctx, "</characteristics>"));
}

static size_t dta_measure_data(readstat_writer_t *writer, dta_ctx_t *ctx) {
    int i;
    for (i=0; i<ctx->nvar; i++) {
        size_t      max_len;
        readstat_variable_t *r_variable = readstat_get_variable(writer, i);
        uint16_t typecode = dta_typecode_for_variable(r_variable, ctx->typlist_version);
        dta_type_info(typecode, &max_len, ctx);
        ctx->record_len += max_len;
    }
    return (dta_measure_tag(ctx, "<data>")
            + ctx->record_len * ctx->nobs
            + dta_measure_tag(ctx, "</data>"));
}

static size_t dta_measure_strls(dta_ctx_t *ctx) {
    return (dta_measure_tag(ctx, "<strls>")
            + dta_measure_tag(ctx, "</strls>"));
}

static size_t dta_measure_value_labels(readstat_writer_t *writer, dta_ctx_t *ctx) {
    size_t len = dta_measure_tag(ctx, "<value_labels>");

    int i, j;
    for (i=0; i<writer->label_sets_count; i++) {
        readstat_label_set_t *r_label_set = readstat_get_label_set(writer, i);
        int32_t n = r_label_set->value_labels_count;
        int32_t txtlen = 0;
        for (j=0; j<n; j++) {
            readstat_value_label_t *value_label = readstat_get_value_label(r_label_set, j);
            txtlen += value_label->label_len + 1;
        }
        len += dta_measure_tag(ctx, "<lbl>");
        len += sizeof(int32_t);
        len += ctx->value_label_table_labname_len;
        len += ctx->value_label_table_padding_len;
        len += 8 + 8*n + txtlen;
        len += dta_measure_tag(ctx, "</lbl>");
    }
    len += dta_measure_tag(ctx, "</value_labels>");
    return len;
}

static readstat_error_t dta_emit_map(readstat_writer_t *writer, dta_ctx_t *ctx) {
    if (!ctx->file_is_xmlish)
        return READSTAT_OK;

    uint64_t map[14];

    map[0] = 0;                                         /* <stata_dta> */
    map[1] = writer->bytes_written;                     /* <map> */
    map[2] = map[1] + dta_measure_map(ctx);             /* <variable_types> */
    map[3] = map[2] + dta_measure_typlist(ctx);         /* <varnames> */
    map[4] = map[3] + dta_measure_varlist(ctx);         /* <sortlist> */
    map[5] = map[4] + dta_measure_srtlist(ctx);         /* <formats> */
    map[6] = map[5] + dta_measure_fmtlist(ctx);         /* <value_label_names> */
    map[7] = map[6] + dta_measure_lbllist(ctx);         /* <variable_labels> */
    map[8] = map[7] + dta_measure_variable_labels(ctx); /* <characteristics> */
    map[9] = map[8] + dta_measure_characteristics(ctx); /* <data> */
    map[10]= map[9] + dta_measure_data(writer, ctx);    /* <strls> */
    map[11]= map[10]+ dta_measure_strls(ctx);           /* <value_labels> */
    map[12]= map[11]+ dta_measure_value_labels(writer, ctx);    /* </stata_dta> */
    map[13]= map[12]+ dta_measure_tag(ctx, "</stata_dta>");

    return dta_write_chunk(writer, ctx, "<map>", map, sizeof(map), "</map>");
}

static readstat_error_t dta_begin_data(void *writer_ctx) {
    readstat_writer_t *writer = (readstat_writer_t *)writer_ctx;
    readstat_error_t error = READSTAT_OK;
    if (!writer->initialized)
        return READSTAT_ERROR_WRITER_NOT_INITIALIZED;
    
    dta_ctx_t *ctx = dta_ctx_alloc(NULL);
    dta_header_t header;
    memset(&header, 0, sizeof(dta_header_t));

    header.ds_format = writer->version;
    header.byteorder = machine_is_little_endian() ? DTA_LOHI : DTA_HILO;
    header.filetype  = 0x01;
    header.unused    = 0x00;
    header.nvar      = writer->variables_count;
    header.nobs      = writer->row_count;

    error = dta_ctx_init(ctx, header.nvar, header.nobs, header.byteorder, header.ds_format, NULL, NULL);
    if (error != READSTAT_OK)
        goto cleanup;
    
    error = dta_emit_header(writer, ctx, &header);
    if (error != READSTAT_OK)
        goto cleanup;

    error = dta_emit_map(writer, ctx);
    if (error != READSTAT_OK)
        goto cleanup;
    
    error = dta_emit_descriptors(writer, ctx);
    if (error != READSTAT_OK)
        goto cleanup;

    error = dta_emit_variable_labels(writer, ctx);
    if (error != READSTAT_OK)
        goto cleanup;

    error = dta_emit_characteristics(writer, ctx);
    if (error != READSTAT_OK)
        goto cleanup;

    error = dta_write_tag(writer, ctx, "<data>");
    if (error != READSTAT_OK)
        goto cleanup;

cleanup:
    if (error != READSTAT_OK) {
        dta_ctx_free(ctx);
    } else {
        writer->module_ctx = ctx;
    }
    
    return error;
}

static readstat_error_t dta_write_raw_char(void *row, char value) {
    memcpy(row, &value, sizeof(char));
    return READSTAT_OK;
}

static readstat_error_t dta_write_raw_int16(void *row, int16_t value) {
    memcpy(row, &value, sizeof(int16_t));
    return READSTAT_OK;
}

static readstat_error_t dta_write_raw_int32(void *row, int32_t value) {
    memcpy(row, &value, sizeof(int32_t));
    return READSTAT_OK;
}

static readstat_error_t dta_write_raw_int64(void *row, int64_t value) {
    memcpy(row, &value, sizeof(int64_t));
    return READSTAT_OK;
}

static readstat_error_t dta_write_raw_float(void *row, float value) {
    memcpy(row, &value, sizeof(float));
    return READSTAT_OK;
}

static readstat_error_t dta_write_raw_double(void *row, double value) {
    memcpy(row, &value, sizeof(double));
    return READSTAT_OK;
}

static readstat_error_t dta_113_write_char(void *row, const readstat_variable_t *var, char value) {
    if (var->type != READSTAT_TYPE_CHAR) {
        return READSTAT_ERROR_VALUE_TYPE_MISMATCH;
    }
    if (value > DTA_113_MAX_CHAR) {
        return READSTAT_ERROR_VALUE_OUT_OF_RANGE;
    }
    return dta_write_raw_char(row, value);
}

static readstat_error_t dta_old_write_char(void *row, const readstat_variable_t *var, char value) {
    if (var->type != READSTAT_TYPE_CHAR) {
        return READSTAT_ERROR_VALUE_TYPE_MISMATCH;
    }
    if (value > DTA_OLD_MAX_CHAR) {
        return READSTAT_ERROR_VALUE_OUT_OF_RANGE;
    }
    return dta_write_raw_char(row, value);
}

static readstat_error_t dta_113_write_int16(void *row, const readstat_variable_t *var, int16_t value) {
    if (var->type != READSTAT_TYPE_INT16) {
        return READSTAT_ERROR_VALUE_TYPE_MISMATCH;
    }
    if (value > DTA_113_MAX_INT16) {
        return READSTAT_ERROR_VALUE_OUT_OF_RANGE;
    }
    return dta_write_raw_int16(row, value);
}

static readstat_error_t dta_old_write_int16(void *row, const readstat_variable_t *var, int16_t value) {
    if (var->type != READSTAT_TYPE_INT16) {
        return READSTAT_ERROR_VALUE_TYPE_MISMATCH;
    }
    if (value > DTA_OLD_MAX_INT16) {
        return READSTAT_ERROR_VALUE_OUT_OF_RANGE;
    }
    return dta_write_raw_int16(row, value);
}

static readstat_error_t dta_113_write_int32(void *row, const readstat_variable_t *var, int32_t value) {
    if (var->type != READSTAT_TYPE_INT32) {
        return READSTAT_ERROR_VALUE_TYPE_MISMATCH;
    }
    if (value > DTA_113_MAX_INT32) {
        return READSTAT_ERROR_VALUE_OUT_OF_RANGE;
    }
    return dta_write_raw_int32(row, value);
}

static readstat_error_t dta_old_write_int32(void *row, const readstat_variable_t *var, int32_t value) {
    if (var->type != READSTAT_TYPE_INT32) {
        return READSTAT_ERROR_VALUE_TYPE_MISMATCH;
    }
    if (value > DTA_OLD_MAX_INT32) {
        return READSTAT_ERROR_VALUE_OUT_OF_RANGE;
    }
    return dta_write_raw_int32(row, value);
}

static readstat_error_t dta_write_float(void *row, const readstat_variable_t *var, float value) {
    if (var->type != READSTAT_TYPE_FLOAT) {
        return READSTAT_ERROR_VALUE_TYPE_MISMATCH;
    }

    int32_t max_flt_i32 = DTA_113_MAX_FLOAT;
    float max_flt;
    memcpy(&max_flt, &max_flt_i32, sizeof(float));
    if (value > max_flt) {
        return READSTAT_ERROR_VALUE_OUT_OF_RANGE;
    } else if (isnan(value)) {
        return dta_113_write_missing_numeric(row, var);
    }
    return dta_write_raw_float(row, value);
}

static readstat_error_t dta_write_double(void *row, const readstat_variable_t *var, double value) {
    if (var->type != READSTAT_TYPE_DOUBLE) {
        return READSTAT_ERROR_VALUE_TYPE_MISMATCH;
    }

    int64_t max_dbl_i64 = DTA_113_MAX_DOUBLE;
    double max_dbl;
    memcpy(&max_dbl, &max_dbl_i64, sizeof(double));
    if (value > max_dbl) {
        return READSTAT_ERROR_VALUE_OUT_OF_RANGE;
    } else if (isnan(value)) {
        return dta_113_write_missing_numeric(row, var);
    }
    return dta_write_raw_double(row, value);
}

static readstat_error_t dta_write_string(void *row, const char *value, size_t max_len) {
    if (value == NULL || value[0] == '\0') {
        memset(row, '\0', max_len);
    } else {
        strncpy((char *)row, value, max_len);
    }
    return READSTAT_OK;
}

static readstat_error_t dta_111_write_string(void *row, const readstat_variable_t *var, 
        const char *value) {
    if (var->type != READSTAT_TYPE_STRING) {
        return READSTAT_ERROR_VALUE_TYPE_MISMATCH;
    }
    size_t value_len = var->storage_width;
    if (value_len > DTA_111_MAX_WIDTH)
        value_len = DTA_111_MAX_WIDTH;
    return dta_write_string(row, value, value_len);
}

static readstat_error_t dta_117_write_string(void *row, const readstat_variable_t *var, 
        const char *value) {
    if (var->type != READSTAT_TYPE_STRING) {
        return READSTAT_ERROR_VALUE_TYPE_MISMATCH;
    }
    size_t value_len = var->storage_width;
    if (value_len > DTA_117_MAX_WIDTH)
        value_len = DTA_117_MAX_WIDTH;
    return dta_write_string(row, value, value_len);
}

static readstat_error_t dta_old_write_string(void *row, const readstat_variable_t *var, 
        const char *value) {
    if (var->type != READSTAT_TYPE_STRING) {
        return READSTAT_ERROR_VALUE_TYPE_MISMATCH;
    }
    size_t value_len = var->storage_width;
    if (value_len > DTA_OLD_MAX_WIDTH)
        value_len = DTA_OLD_MAX_WIDTH;
    return dta_write_string(row, value, value_len);
}

static readstat_error_t dta_113_write_missing_numeric(void *row, const readstat_variable_t *var) {
    readstat_error_t retval = READSTAT_OK;
    if (var->type == READSTAT_TYPE_CHAR) {
        retval = dta_write_raw_char(row, DTA_113_MISSING_CHAR);
    } else if (var->type == READSTAT_TYPE_INT16) {
        retval = dta_write_raw_int16(row, DTA_113_MISSING_INT16);
    } else if (var->type == READSTAT_TYPE_INT32) {
        retval = dta_write_raw_int32(row, DTA_113_MISSING_INT32);
    } else if (var->type == READSTAT_TYPE_FLOAT) {
        retval = dta_write_raw_int32(row, DTA_113_MISSING_FLOAT);
    } else if (var->type == READSTAT_TYPE_DOUBLE) {
        retval = dta_write_raw_int64(row, DTA_113_MISSING_DOUBLE);
    }
    return retval;
}

static readstat_error_t dta_old_write_missing_numeric(void *row, const readstat_variable_t *var) {
    readstat_error_t retval = READSTAT_OK;
    if (var->type == READSTAT_TYPE_CHAR) {
        retval = dta_write_raw_char(row, DTA_OLD_MISSING_CHAR);
    } else if (var->type == READSTAT_TYPE_INT16) {
        retval = dta_write_raw_int16(row, DTA_OLD_MISSING_INT16);
    } else if (var->type == READSTAT_TYPE_INT32) {
        retval = dta_write_raw_int32(row, DTA_OLD_MISSING_INT32);
    } else if (var->type == READSTAT_TYPE_FLOAT) {
        retval = dta_write_raw_int32(row, DTA_OLD_MISSING_FLOAT);
    } else if (var->type == READSTAT_TYPE_DOUBLE) {
        retval = dta_write_raw_int64(row, DTA_OLD_MISSING_DOUBLE);
    }
    return retval;
}

static readstat_error_t dta_111_write_missing_string(void *row, const readstat_variable_t *var) {
    return dta_111_write_string(row, var, NULL);
}

static readstat_error_t dta_117_write_missing_string(void *row, const readstat_variable_t *var) {
    return dta_117_write_string(row, var, NULL);
}

static readstat_error_t dta_old_write_missing_string(void *row, const readstat_variable_t *var) {
    return dta_old_write_string(row, var, NULL);
}

static readstat_error_t dta_113_write_missing_tagged(void *row, const readstat_variable_t *var, char tag) {
    readstat_error_t retval = READSTAT_OK;
    if (tag < 'a' || tag > 'z')
        return READSTAT_ERROR_VALUE_OUT_OF_RANGE;

    if (var->type == READSTAT_TYPE_CHAR) {
        retval = dta_write_raw_char(row, DTA_113_MISSING_CHAR_A + (tag - 'a'));
    } else if (var->type == READSTAT_TYPE_INT16) {
        retval = dta_write_raw_int16(row, DTA_113_MISSING_INT16_A + (tag - 'a'));
    } else if (var->type == READSTAT_TYPE_INT32) {
        retval = dta_write_raw_int32(row, DTA_113_MISSING_INT32_A + (tag - 'a'));
    } else if (var->type == READSTAT_TYPE_FLOAT) {
        retval = dta_write_raw_int32(row, DTA_113_MISSING_FLOAT_A + ((tag - 'a') << 11));
    } else if (var->type == READSTAT_TYPE_DOUBLE) {
        retval = dta_write_raw_int64(row, DTA_113_MISSING_DOUBLE_A + ((int64_t)(tag - 'a') << 40));
    } else {
        retval = READSTAT_ERROR_TAGGED_VALUES_NOT_SUPPORTED;
    }
    return retval;
}

static readstat_error_t dta_old_write_missing_tagged(void *row, const readstat_variable_t *var, char tag) {
    /* Write it anyway, but return an error */
    dta_old_write_missing_numeric(row, var);
    return READSTAT_ERROR_TAGGED_VALUES_NOT_SUPPORTED;
}

static readstat_error_t dta_end_data(void *writer_ctx) {
    readstat_writer_t *writer = (readstat_writer_t *)writer_ctx;
    dta_ctx_t *ctx = writer->module_ctx;
    readstat_error_t error = READSTAT_OK;

    if (!writer->initialized)
        return READSTAT_ERROR_WRITER_NOT_INITIALIZED;

    error = dta_write_tag(writer, ctx, "</data>");
    if (error != READSTAT_OK)
        goto cleanup;

    error = dta_emit_strls(writer, ctx);
    if (error != READSTAT_OK)
        goto cleanup;

    error = dta_emit_value_labels(writer, ctx);
    if (error != READSTAT_OK)
        goto cleanup;

    error = dta_write_tag(writer, ctx, "</stata_dta>");
    if (error != READSTAT_OK)
        goto cleanup;

cleanup:
    dta_ctx_free(writer->module_ctx);
    writer->module_ctx = NULL;

    return error;
}

readstat_error_t readstat_begin_writing_dta(readstat_writer_t *writer, void *user_ctx, long row_count) {
    writer->row_count = row_count;
    writer->user_ctx = user_ctx;

    if (writer->version == 0)
        writer->version = DTA_DEFAULT_FILE_VERSION;

    if (writer->version >= 119 || writer->version < 104) {
        return READSTAT_ERROR_UNSUPPORTED_FILE_FORMAT_VERSION;
    } else if (writer->version >= 117) {
        writer->callbacks.variable_width = &dta_117_variable_width;
        writer->callbacks.write_string = &dta_117_write_string;
        writer->callbacks.write_missing_string = &dta_117_write_missing_string;
    } else if (writer->version >= 111) {
        writer->callbacks.variable_width = &dta_111_variable_width;
        writer->callbacks.write_string = &dta_111_write_string;
        writer->callbacks.write_missing_string = &dta_111_write_missing_string;
    } else {
        writer->callbacks.variable_width = &dta_old_variable_width;
        writer->callbacks.write_string = &dta_old_write_string;
        writer->callbacks.write_missing_string = &dta_old_write_missing_string;
    }

    if (writer->version >= 113) {
        writer->callbacks.write_char = &dta_113_write_char;
        writer->callbacks.write_int16 = &dta_113_write_int16;
        writer->callbacks.write_int32 = &dta_113_write_int32;
        writer->callbacks.write_missing_number = &dta_113_write_missing_numeric;
        writer->callbacks.write_missing_tagged = &dta_113_write_missing_tagged;
    } else {
        writer->callbacks.write_char = &dta_old_write_char;
        writer->callbacks.write_int16 = &dta_old_write_int16;
        writer->callbacks.write_int32 = &dta_old_write_int32;
        writer->callbacks.write_missing_number = &dta_old_write_missing_numeric;
        writer->callbacks.write_missing_tagged = &dta_old_write_missing_tagged;
    }

    writer->callbacks.write_float = &dta_write_float;
    writer->callbacks.write_double = &dta_write_double;

    writer->callbacks.begin_data = &dta_begin_data;
    writer->callbacks.end_data = &dta_end_data;
    writer->initialized = 1;

    return READSTAT_OK;
}
