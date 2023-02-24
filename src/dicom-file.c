/*
 * Implementation of Part 10 of the DICOM standard: Media Storage and File
 * Format for Media Interchange.
 */

#include "config.h"

#ifdef _WIN32
// the Windows CRT considers strdup and strcpy unsafe
#define _CRT_SECURE_NO_WARNINGS
// and deprecates strdup
#define strdup(v) _strdup(v)
#endif

#include <assert.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>

#include "utarray.h"

#include <dicom/dicom.h>
#include "pdicom.h"


#define TAG_ITEM                  0xFFFEE000
#define TAG_ITEM_DELIM            0xFFFEE00D
#define TAG_SQ_DELIM              0xFFFEE0DD
#define TAG_TRAILING_PADDING      0xFFFCFFFC
#define TAG_EXTENDED_OFFSET_TABLE 0x7FE00001
#define TAG_PIXEL_DATA            0x7FE00010
#define TAG_FLOAT_PIXEL_DATA      0x7FE00008
#define TAG_DOUBLE_PIXEL_DATA     0x7FE00009

struct PixelDescription {
    uint16_t rows;
    uint16_t columns;
    uint16_t samples_per_pixel;
    uint16_t bits_allocated;
    uint16_t bits_stored;
    uint16_t high_bit;
    uint16_t pixel_representation;
    uint16_t planar_configuration;
    char *photometric_interpretation;
};


typedef struct ItemHeader {
    uint32_t tag;
    uint64_t length;
} IHeader;


typedef struct ElementHeader {
    uint32_t tag;
    char vr[3];
    uint64_t length;
} EHeader;


struct _DcmFilehandle {
    DcmIO *io;
    void *fp;
    DcmDataSet *meta;
    int64_t offset;
    char *transfer_syntax_uid;
    int64_t pixel_data_offset;
    uint64_t *extended_offset_table;
    bool big_endian;
};


/* TRUE for big-endian machines, like PPC. We need to byteswap DICOM
 * numeric types in this case. Run time tests for this are much
 * simpler to manage when cross-compiling.
 */
static bool is_big_endian(void)
{
    union {
        uint32_t i;
        char c[4];
    } bint = {0x01020304};

    return bint.c[0] == 1;
}


DcmFilehandle *dcm_filehandle_create(DcmError **error, DcmIO *io, void *client)
{
    DcmFilehandle *filehandle = DCM_NEW(error, DcmFilehandle);
    if (filehandle == NULL) {
        return NULL;
    }

    filehandle->io = io;
    filehandle->fp = NULL;
    filehandle->meta = NULL;
    filehandle->offset = 0;
    filehandle->transfer_syntax_uid = NULL;
    filehandle->pixel_data_offset = 0;
    filehandle->extended_offset_table = NULL;
    filehandle->big_endian = is_big_endian();

    filehandle->fp = filehandle->io->open(error, client);
    if (filehandle->fp == NULL) {
        dcm_filehandle_destroy(filehandle);
        return NULL;
    }

    return filehandle;
}


void dcm_filehandle_destroy(DcmFilehandle *filehandle)
{
    if (filehandle) {
        if (filehandle->transfer_syntax_uid) {
            free(filehandle->transfer_syntax_uid);
        }

        if (filehandle->fp) {
            (void)filehandle->io->close(NULL, filehandle->fp);
            filehandle->fp = NULL;
        }

        free(filehandle);
    }
}


static int64_t dcm_read(DcmError **error, DcmFilehandle *filehandle,
    char *buffer, int64_t length, int64_t *position)
{
    int64_t bytes_read = filehandle->io->read(error, 
                                              filehandle->fp, buffer, length);
    if (bytes_read < 0) {
        return bytes_read;
    }

    *position += bytes_read;

    return bytes_read;
}


static bool dcm_require(DcmError **error, DcmFilehandle *filehandle, 
    char *buffer, int64_t length, int64_t *position)
{
    while (length > 0) {
        int64_t bytes_read = dcm_read(error, 
                                      filehandle, buffer, length, position);

        if (bytes_read < 0) {
            return false;
        } else if (bytes_read == 0) {
            dcm_error_set(error, DCM_ERROR_CODE_IO,
                "End of filehandle",
                "Needed %zd bytes beyond end of filehandle", length);
            return false;
        }

        buffer += bytes_read;
        length -= bytes_read;
    }

    return true;
}


static bool dcm_seekset(DcmError **error, 
                        DcmFilehandle *filehandle, int64_t offset)
{
    return filehandle->io->seek(error, filehandle->fp, offset, SEEK_SET) >= 0;
}


static bool dcm_seekcur(DcmError **error, DcmFilehandle *filehandle, 
    int64_t offset, int64_t *position)
{
    int64_t new_offset = filehandle->io->seek(error, 
                                              filehandle->fp, offset, SEEK_CUR);
    if (new_offset < 0) {
        return false;
    }

    *position += offset;

    return true;
}


static bool dcm_offset(DcmError **error, 
                       DcmFilehandle *filehandle, int64_t *offset)
{
    *offset = filehandle->io->seek(error, filehandle->fp, 0, SEEK_CUR);
    if (*offset < 0) {
        return false;
    }

    return true;
}


static bool dcm_is_eof(DcmFilehandle *filehandle)
{
    int64_t position = 0;
    bool eof = true;

    char buffer[1];
    int64_t bytes_read = filehandle->io->read(NULL, filehandle->fp, buffer, 1);
    if (bytes_read > 0) {
        eof = false;
        (void) dcm_seekcur(NULL, filehandle, -1, &position);
    }

    return eof;
}


static bool read_uint16(DcmError **error, DcmFilehandle *filehandle, 
    uint16_t *value, int64_t *position)
{
    union {
        uint16_t i;
        char c[2];
    } buffer;

    if (!dcm_require(error, filehandle, buffer.c, 2, position)) {
        return false;
    }

    if (filehandle->big_endian) {
        buffer.i = (uint32_t)buffer.c[0] | 
            ((uint32_t)buffer.c[1] << 8);
    }

    *value = buffer.i;

    return true;
}


static bool read_uint32(DcmError **error, DcmFilehandle *filehandle, 
    uint32_t *value, int64_t *position)
{
    union {
        uint32_t i;
        char c[4];
    } buffer;

    if (!dcm_require(error, filehandle, buffer.c, 4, position)) {
        return false;
    }

    if (filehandle->big_endian) {
        buffer.i = (uint32_t)buffer.c[0] | 
            ((uint32_t)buffer.c[1] << 8) |
            ((uint32_t)buffer.c[2] << 16) | 
            ((uint32_t)buffer.c[3] << 24);
    }

    *value = buffer.i;

    return true;
}


static bool read_uint64(DcmError **error, DcmFilehandle *filehandle, 
    uint64_t *value, int64_t *position)
{
    union {
        uint64_t l;
        char c[8];
    } buffer;

    if (!dcm_require(error, filehandle, buffer.c, 8, position)) {
        return false;
    }

    if (filehandle->big_endian) {
        buffer.l = (uint64_t)buffer.c[0] | 
            ((uint64_t)buffer.c[1] << 8) |
            ((uint64_t)buffer.c[2] << 16) | 
            ((uint64_t)buffer.c[3] << 24) | 
            ((uint64_t)buffer.c[4] << 32) | 
            ((uint64_t)buffer.c[5] << 40) | 
            ((uint64_t)buffer.c[6] << 48) | 
            ((uint64_t)buffer.c[7] << 56);
    }

    *value = buffer.l;

    return true;
}


static bool read_float(DcmError **error, DcmFilehandle *filehandle, 
    float *value, int64_t *position)
{
    union {
        uint32_t i;
        float f;
    } buffer;

    if (!read_uint32(error, filehandle, &buffer.i, position)) {
        return false;
    }

    *value = buffer.f;

    return true;
}


static bool read_double(DcmError **error, DcmFilehandle *filehandle, 
    double *value, int64_t *position)
{
    union {
        uint64_t l;
        double d;
    } buffer;

    if (!read_uint64(error, filehandle, &buffer.l, position)) {
        return false;
    }

    *value = buffer.l;

    return true;
}


static bool read_tag(DcmError **error, DcmFilehandle *filehandle, 
    uint32_t *value, int64_t *position)
{
    uint16_t group, elem;

    if (!read_uint16(error, filehandle, &group, position) ||
        !read_uint16(error, filehandle, &elem, position)) {
        return false;
    }

    *value = ((uint32_t)group << 16) | elem;

    return true;
}


static IHeader *iheader_create(DcmError **error, 
    uint32_t tag, uint64_t length)
{
    if (tag != TAG_ITEM &&
        tag != TAG_ITEM_DELIM &&
        tag != TAG_SQ_DELIM) {
        dcm_error_set(error, DCM_ERROR_CODE_INVALID,
                      "Constructing header of Item failed",
                      "Invalid Item Tag '%08X'",
                      tag);
        return NULL;
    }
    IHeader *header = DCM_NEW(error, struct ItemHeader);
    if (header == NULL) {
        return NULL;
    }
    header->tag = tag;
    header->length = length;
    return header;
}


static uint32_t iheader_get_tag(IHeader *item)
{
    assert(item);
    return item->tag;
}


static uint64_t iheader_get_length(IHeader *item)
{
    assert(item);
    return item->length;
}


static void iheader_destroy(IHeader *item)
{
    if (item) {
        free(item);
        item = NULL;
    }
}


static EHeader *eheader_create(DcmError **error, 
    uint32_t tag, const char *vr, uint64_t length)
{
    if (!dcm_is_valid_tag(tag)) {
        dcm_error_set(error, DCM_ERROR_CODE_INVALID,
                      "Constructing header of Data Element failed",
                      "Invalid Tag: '%08X'",
                      tag);
        return NULL;
    }
    if (!dcm_is_valid_vr(vr)) {
        dcm_error_set(error, DCM_ERROR_CODE_INVALID,
                      "Constructing header of Data Element failed",
                      "Invalid Value Representation: '%s'",
                      vr);
        return NULL;
    }

    EHeader *header = DCM_NEW(error, struct ElementHeader);
    if (header == NULL) {
        return NULL;
    }
    header->tag = tag;
    strncpy(header->vr, vr, 3);
    header->vr[2] = '\0';
    header->length = length;

    return header;
}


static uint16_t eheader_get_group_number(EHeader *header)
{
    assert(header);
    return (uint16_t)(header->tag >> 16);
}


static uint32_t eheader_get_tag(EHeader *header)
{
    assert(header);
    return header->tag;
}


static bool eheader_check_vr(EHeader *header, const char *vr)
{
    assert(header);
    return (strcmp(header->vr, vr) == 0);
}


static uint64_t eheader_get_length(EHeader *header)
{
    assert(header);
    return header->length;
}


static void eheader_destroy(EHeader *header)
{
    if (header) {
        free(header);
        header = NULL;
    }
}


char **dcm_parse_character_string(DcmError **error, 
                                  char *string, uint32_t *vm)
{
    uint32_t i;
    uint32_t n;
    char **token_ptr;
    char **parts;
    char *token;

    UT_array *array;
    utarray_new(array, &ut_str_icd);

    if (strlen(string) == 0) {
        token = "";
        utarray_push_back(array, &token);
        goto finish;
    }
    token = strtok(string, "\\");
    while(token) {
        utarray_push_back(array, &token);
        token = strtok(NULL, "\\");
    }
    goto finish;

finish:
    n = utarray_len(array);
    parts = DCM_NEW_ARRAY(error, n, char *);
    if (parts == NULL) {
        free(string);
        utarray_free(array);
        return NULL;
    }

    for (i = 0; i < n; i++) {
        token_ptr = utarray_eltptr(array, i);
        parts[i] = dcm_strdup(error, *token_ptr);
        if (parts[i] == NULL) {
            free(parts);
            free(string);
            utarray_free(array);
            return NULL;
        }
    }

    *vm = n;
    utarray_free(array);
    free(string);
    return parts;
}


static IHeader *read_item_header(DcmError **error, 
    DcmFilehandle *filehandle, int64_t *position)
{
    // length is only 32 bits in a DICOM filehandle, though we use 64 internally
    uint32_t tag;
    uint32_t length;
    if (!read_tag(error, filehandle, &tag, position) ||
        !read_uint32(error, filehandle, &length, position)) {
        return NULL;
    }

    return iheader_create(error, tag, length);
}


static EHeader *read_element_header(DcmError **error, 
    DcmFilehandle *filehandle, int64_t *position, bool implicit)
{
    char vr[3];
    uint32_t length;

    uint32_t tag;
    if (!read_tag(error, filehandle, &tag, position)) {
        return NULL;
    }

    if (implicit) {
        // Value Representation
        const char *tmp = dcm_dict_lookup_vr(tag);
        strncpy(vr, tmp, 2);
        vr[2] = '\0';

        // Value Length
        if (!read_uint32(error, filehandle, &length, position)) {
            return NULL;
        }
    } else {
        // Value Representation
        if (!dcm_require(error, filehandle, vr, 2, position)) {
            return NULL;
        }
        vr[2] = '\0';

        // Value Length
        // FIXME oooof look up the VR and get the size from some kind of table
        if (strcmp(vr, "AE") == 0 ||
            strcmp(vr, "AS") == 0 ||
            strcmp(vr, "AT") == 0 ||
            strcmp(vr, "CS") == 0 ||
            strcmp(vr, "DA") == 0 ||
            strcmp(vr, "DS") == 0 ||
            strcmp(vr, "DT") == 0 ||
            strcmp(vr, "FL") == 0 ||
            strcmp(vr, "FD") == 0 ||
            strcmp(vr, "IS") == 0 ||
            strcmp(vr, "LO") == 0 ||
            strcmp(vr, "LT") == 0 ||
            strcmp(vr, "PN") == 0 ||
            strcmp(vr, "SH") == 0 ||
            strcmp(vr, "SL") == 0 ||
            strcmp(vr, "SS") == 0 ||
            strcmp(vr, "ST") == 0 ||
            strcmp(vr, "TM") == 0 ||
            strcmp(vr, "UI") == 0 ||
            strcmp(vr, "UL") == 0 ||
            strcmp(vr, "US") == 0) {
            // These VRs have a short length of only two bytes
            uint16_t short_length;
            if (!read_uint16(error, filehandle, &short_length, position)) {
                return NULL;
            }
            length = (uint32_t) short_length;
        } else {
            // Other VRs have two reserved bytes before length of four bytes
            uint16_t reserved;
            if (!read_uint16(error, filehandle, &reserved, position) ||
                !read_uint32(error, filehandle, &length, position)) {
               return NULL;
            }

            if (reserved != 0x0000) {
                dcm_error_set(error, DCM_ERROR_CODE_PARSE,
                              "Reading of Data Element header failed",
                              "Unexpected value for reserved bytes "
                              "of Data Element %08X with VR '%s'.",
                              tag, vr);
                return NULL;
            }
        }
    }

    return eheader_create(error, tag, vr, length);
}


static DcmElement *read_element(DcmError **error,
                                DcmFilehandle *filehandle,
                                EHeader *header,
                                int64_t *position,
                                bool implicit)
{
    assert(header);
    uint32_t vm;
    uint32_t item_tag;
    uint32_t item_length;
    DcmElement *element;
    IHeader *item_iheader;
    DcmDataSet *item_dataset;
    EHeader *item_eheader;
    DcmElement *item_element;

    uint32_t tag = eheader_get_tag(header);
    uint32_t length = eheader_get_length(header);

    dcm_log_debug("Read Data Element '%08X'", tag);

    vm = 1;
    // Character strings
    // FIXME ... look up the vr and get this from a table somewhere
    if (eheader_check_vr(header, "AE") ||
        eheader_check_vr(header, "AS") ||
        eheader_check_vr(header, "AT") ||
        eheader_check_vr(header, "CS") ||
        eheader_check_vr(header, "DA") ||
        eheader_check_vr(header, "DS") ||  // Decimal String
        eheader_check_vr(header, "DT") ||
        eheader_check_vr(header, "IS") ||  // Integer String
        eheader_check_vr(header, "LO") ||
        eheader_check_vr(header, "PN") ||
        eheader_check_vr(header, "SH") ||
        eheader_check_vr(header, "TM") ||
        eheader_check_vr(header, "UI") ||
        eheader_check_vr(header, "UR")) {
        char *value = DCM_MALLOC(error, length + 1);
        if (value == NULL) {
            return NULL;
        }

        if (!dcm_require(error, filehandle, value, length, position)) {
            free(value);
            return NULL;
        }

        /* C-style \0 terminate.
         */
        if (length > 0) {
            if (!eheader_check_vr(header, "UI")) {
                if (isspace(value[length - 1])) {
                    value[length - 1] = '\0';
                }
            }
        }
        value[length] = '\0';

        // Parse value and create array of strings
        char **strings = dcm_parse_character_string(error, value, &vm);

        if (eheader_check_vr(header, "AE")) {
            return dcm_element_create_AE_multi(error, tag, strings, vm);
        } else if (eheader_check_vr(header, "AS")) {
            return dcm_element_create_AS_multi(error, tag, strings, vm);
        } else if (eheader_check_vr(header, "AT")) {
            return dcm_element_create_AT_multi(error, tag, strings, vm);
        } else if (eheader_check_vr(header, "CS")) {
            return dcm_element_create_CS_multi(error, tag, strings, vm);
        } else if (eheader_check_vr(header, "DA")) {
            return dcm_element_create_DA_multi(error, tag, strings, vm);
        } else if (eheader_check_vr(header, "DS")) {
            return dcm_element_create_DS_multi(error, tag, strings, vm);
        } else if (eheader_check_vr(header, "DT")) {
            return dcm_element_create_DT_multi(error, tag, strings, vm);
        } else if (eheader_check_vr(header, "IS")) {
            return dcm_element_create_IS_multi(error, tag, strings, vm);
        } else if (eheader_check_vr(header, "LO")) {
            return dcm_element_create_LO_multi(error, tag, strings, vm);
        } else if (eheader_check_vr(header, "PN")) {
            return dcm_element_create_PN_multi(error, tag, strings, vm);
        } else if (eheader_check_vr(header, "SH")) {
            return dcm_element_create_SH_multi(error, tag, strings, vm);
        } else if (eheader_check_vr(header, "TM")) {
            return dcm_element_create_TM_multi(error, tag, strings, vm);
        } else if (eheader_check_vr(header, "UI")) {
            return dcm_element_create_UI_multi(error, tag, strings, vm);
        } else {
            dcm_error_set(error, DCM_ERROR_CODE_PARSE,
                          "Reading of Data Element failed",
                          "Encountered unexpected Value Representation "
                          "for Data Element '%08X'",
                          tag);
            dcm_free_string_array(strings, vm);
            return NULL;
        }
    } else if (eheader_check_vr(header, "UT") ||
        eheader_check_vr(header, "LT") ||
        eheader_check_vr(header, "ST") ||
        eheader_check_vr(header, "UR")) {
        // unparsed character strings
        char *value = DCM_MALLOC(error, length + 1);
        if (value == NULL) {
            return NULL;
        }

        if (!dcm_require(error, filehandle, value, length, position)) {
            free(value);
            return NULL;
        }

        /* C-style \0 terminate.
         */
        value[length] = '\0';

        if (eheader_check_vr(header, "UT")) {
            return dcm_element_create_UT(error, tag, value);
        } else if (eheader_check_vr(header, "LT")) {
            return dcm_element_create_LT(error, tag, value);
        } else if (eheader_check_vr(header, "ST")) {
            return dcm_element_create_ST(error, tag, value);
        } else if (eheader_check_vr(header, "UR")) {
            return dcm_element_create_UR(error, tag, value);
        } else {
            dcm_error_set(error, DCM_ERROR_CODE_PARSE,
                          "Reading of Data Element failed",
                          "Encountered unexpected Value Representation "
                          "for Data Element '%08X'",
                          tag);
            free(value);
            return NULL;
        }
    } else if (eheader_check_vr(header, "SQ")) {
        vm = 1;
        DcmSequence *value = dcm_sequence_create(error);
        if (value == NULL) {
            return NULL;
        }
        if (length == 0) {
            return dcm_element_create_SQ(error, tag, value);
        } else if (length == 0xFFFFFFFF) {
            dcm_log_debug("Sequence of Data Element '%08X' "
                          "has undefined length.",
                          tag);
        } else {
            dcm_log_debug("Sequence of Data Element '%08X' "
                          "has defined length %d.",
                          tag, length);
        }

        int64_t seq_position = 0;
        int item_index = 0;
        while (seq_position < length) {
            dcm_log_debug("Read Item #%d of Data Element '%08X'.",
                          item_index, tag);
            item_iheader = read_item_header(error, filehandle, &seq_position);
            if (item_iheader == NULL) {
                iheader_destroy(item_iheader);
                dcm_sequence_destroy(value);
                return NULL;
            }
            item_tag = iheader_get_tag(item_iheader);
            item_length = iheader_get_length(item_iheader);
            if (item_tag == TAG_SQ_DELIM) {
                dcm_log_debug("Stop reading Data Element '%08X'. "
                              "Encountered Sequence Delimination Tag.",
                              tag);
                iheader_destroy(item_iheader);
                break;
            }
            if (item_tag != TAG_ITEM) {
                dcm_error_set(error, DCM_ERROR_CODE_PARSE,
                              "Reading of Data Element failed",
                              "Expected tag '%08X' instead of '%08X' "
                              "for Item #%d of Data Element '%08X'",
                              TAG_ITEM,
                              item_tag,
                              item_index,
                              tag);
                iheader_destroy(item_iheader);
                dcm_sequence_destroy(value);
                return NULL;
            } else if (item_length == 0xFFFFFFFF) {
                dcm_log_debug("Item #%d of Data Element '%08X' "
                              "has undefined length.",
                              item_index, tag);
            } else {
                dcm_log_debug("Item #%d of Data Element '%08X' "
                              "has defined length %d.",
                              item_index, tag, item_length);
            }

            item_dataset = dcm_dataset_create(error);
            if (item_dataset == NULL) {
                iheader_destroy(item_iheader);
                dcm_sequence_destroy(value);
                return NULL;
            }

            int64_t item_position = 0;
            while (item_position < item_length) {
                /* Peek the next tag.
                 */
                uint32_t item_tag;
                if (!read_tag(error, filehandle, &item_tag, &item_position)) {
                    return NULL;
                }

                if (item_tag == TAG_ITEM_DELIM) {
                    // skip the tag length
                    uint32_t item_length;
                    dcm_log_debug("Stop reading Item #%d of "
                                  "Data Element '%08X'. "
                                  "Encountered Item Delimination Tag.",
                                  item_index, tag);
                    if (!read_uint32(error, filehandle, 
                        &item_length, &item_position)) {
                        iheader_destroy(item_iheader);
                        dcm_sequence_destroy(value);
                        return NULL;
                    }
                            
                    break;
                }

                if (!dcm_seekcur(error, filehandle, -4, &item_position)) {
                    iheader_destroy(item_iheader);
                    dcm_sequence_destroy(value);
                    return NULL;
                }

                item_eheader = read_element_header(error, filehandle, 
                                                   &item_position, implicit);
                if (item_eheader == NULL) {
                    iheader_destroy(item_iheader);
                    dcm_sequence_destroy(value);
                    return NULL;
                }

                item_element = read_element(error, filehandle, item_eheader,
                                            &item_position, implicit);
                if (item_element == NULL) {
                    iheader_destroy(item_iheader);
                    eheader_destroy(item_eheader);
                    dcm_sequence_destroy(value);
                    return NULL;
                }

                if (!dcm_dataset_insert(error, item_dataset, item_element)) {
                    iheader_destroy(item_iheader);
                    eheader_destroy(item_eheader);
                    dcm_sequence_destroy(value);
                    return NULL;
                }
                eheader_destroy(item_eheader);
            }

            seq_position += item_position;

            dcm_sequence_append(error, value, item_dataset);
            iheader_destroy(item_iheader);
            item_index += 1;
        }
        *position += seq_position;
        return dcm_element_create_SQ(error, tag, value);
    } else if (eheader_check_vr(header, "FD")) {
        vm = length / sizeof(double);
        double *values = DCM_NEW_ARRAY(error, vm, double);
        if (values == NULL) {
            return NULL;
        }

        for (uint32_t i = 0; i < vm; i++) {
            double val;
            if (!read_double(error, filehandle, &val, position)) {
                free(values);
                return NULL;
            }

            values[i] = val;
        }

        return dcm_element_create_FD_multi(error, tag, values, vm);
    } else if (eheader_check_vr(header, "FL")) {
        vm = length / sizeof(float);
        float *values = DCM_NEW_ARRAY(error, vm, float);
        if (values == NULL) {
            return NULL;
        }

        for (uint32_t i = 0; i < vm; i++) {
            float val;
            if (!read_float(error, filehandle, &val, position)) {
                free(values);
                return NULL;
            }

            values[i] = val;
        }

        return dcm_element_create_FL_multi(error, tag, values, vm);
    } else if (eheader_check_vr(header, "SS")) {
        vm = length / sizeof(int16_t);
        int16_t *values = DCM_NEW_ARRAY(error, vm, int16_t);
        if (values == NULL) {
            return NULL;
        }

        for (uint32_t i = 0; i < vm; i++) {
            uint16_t val;
            if (!read_uint16(error, filehandle, &val, position)) {
                free(values);
                return NULL;
            }

            values[i] = val;
        }

        return dcm_element_create_SS_multi(error, tag, values, vm);
    } else if (eheader_check_vr(header, "US")) {
        vm = length / sizeof(uint16_t);
        uint16_t *values = DCM_NEW_ARRAY(error, vm, uint16_t);
        if (values == NULL) {
            return NULL;
        }

        for (uint32_t i = 0; i < vm; i++) {
            uint16_t val;
            if (!read_uint16(error, filehandle, &val, position)) {
                free(values);
                return NULL;
            }

            values[i] = val;
        }

        return dcm_element_create_US_multi(error, tag, values, vm);
    } else if (eheader_check_vr(header, "SL")) {
        vm = length / sizeof(int32_t);
        int32_t *values = DCM_NEW_ARRAY(error, vm, int32_t);
        if (values == NULL) {
            return NULL;
        }

        for (uint32_t i = 0; i < vm; i++) {
            uint32_t val;
            if (!read_uint32(error, filehandle, &val, position)) {
                free(values);
                return NULL;
            }

            values[i] = val;
        }

        return dcm_element_create_SL_multi(error, tag, values, vm);
    } else if (eheader_check_vr(header, "UL")) {
        vm = length / sizeof(uint32_t);
        uint32_t *values = DCM_NEW_ARRAY(error, vm, uint32_t);
        if (values == NULL) {
            return NULL;
        }

        for (uint32_t i = 0; i < vm; i++) {
            uint32_t val;
            if (!read_uint32(error, filehandle, &val, position)) {
                free(values);
                return NULL;
            }

            values[i] = val;
        }

        return dcm_element_create_UL_multi(error, tag, values, vm);
    } else if (eheader_check_vr(header, "SV")) {
        vm = length / sizeof(int64_t);
        int64_t *values = DCM_NEW_ARRAY(error, vm, int64_t);
        if (values == NULL) {
            return NULL;
        }

        for (uint32_t i = 0; i < vm; i++) {
            uint64_t val;
            if (!read_uint64(error, filehandle, &val, position)) {
                free(values);
                return NULL;
            }

            values[i] = val;
        }

        return dcm_element_create_SV_multi(error, tag, values, vm);
    } else if (eheader_check_vr(header, "UV")) {
        vm = length / sizeof(uint64_t);
        uint64_t *values = DCM_NEW_ARRAY(error, vm, uint64_t);
        if (values == NULL) {
            return NULL;
        }

        for (uint32_t i = 0; i < vm; i++) {
            uint64_t val;
            if (!read_uint64(error, filehandle, &val, position)) {
                free(values);
                return NULL;
            }

            values[i] = val;
        }

        return dcm_element_create_UV_multi(error, tag, values, vm);
    } else {
        vm = 1;
        char *value = DCM_MALLOC(error, length);
        if (value == NULL) {
            return NULL;
        }

        if (!dcm_require(error, filehandle, value, length, position)) {
            free(value);
            return NULL;
        }

        if (eheader_check_vr(header, "OB")) {
            return dcm_element_create_OB(error, tag, value, length);
        } else if (eheader_check_vr(header, "OD")) {
            return dcm_element_create_OD(error, tag, value, length);
        } else if (eheader_check_vr(header, "OF")) {
            return dcm_element_create_OF(error, tag, value, length);
        } else if (eheader_check_vr(header, "OL")) {
            return dcm_element_create_OL(error, tag, value, length);
        } else if (eheader_check_vr(header, "OV")) {
            return dcm_element_create_OV(error, tag, value, length);
        } else if (eheader_check_vr(header, "OW")) {
            return dcm_element_create_OW(error, tag, value, length);
        } else if (eheader_check_vr(header, "UC")) {
            return dcm_element_create_UC(error, tag, value, length);
        } else if (eheader_check_vr(header, "UN")) {
            return dcm_element_create_UN(error, tag, value, length);
        } else {
            tag = eheader_get_tag(header);
            dcm_error_set(error, DCM_ERROR_CODE_PARSE,
                          "Reading of Data Element failed",
                          "Data Element '%08X' has unexpected "
                          "Value Representation", tag);
            return NULL;
        }
    }

    return element;
}


DcmDataSet *dcm_filehandle_read_filehandle_meta(DcmError **error, DcmFilehandle *filehandle)
{
    const bool implicit = false;

    int64_t position;
    uint32_t tag;
    uint16_t group_number;
    EHeader *header;
    DcmElement *element;

    DcmDataSet *filehandle_meta = dcm_dataset_create(error);
    if (filehandle_meta == NULL) {
        return NULL;
    }

    position = 0;

    // File Preamble
    char preamble[129];
    if (!dcm_require(error, 
                     filehandle, preamble, sizeof(preamble) - 1, &position)) {
        dcm_dataset_destroy(filehandle_meta);
        return NULL;
    }

    preamble[128] = '\0';

    // DICOM Prefix
    char prefix[5];
    if (!dcm_require(error, 
                     filehandle, prefix, sizeof(prefix) - 1, &position)) {
        dcm_dataset_destroy(filehandle_meta);
        return NULL;
    }
    prefix[4] = '\0';

    if (strcmp(prefix, "DICM") != 0) {
        dcm_error_set(error, DCM_ERROR_CODE_PARSE,
                      "Reading of File Meta Information failed",
                      "Prefix 'DICM' not found.");
        dcm_dataset_destroy(filehandle_meta);
        return NULL;
    }

    position = 0;

    // File Meta Information Group Length
    header = read_element_header(error, filehandle, &position, implicit);
    if (header == NULL) {
        dcm_dataset_destroy(filehandle_meta);
        return NULL;
    }

    element = read_element(error, filehandle, header, &position, implicit);
    if (element == NULL) {
        eheader_destroy(header);
        dcm_dataset_destroy(filehandle_meta);
        return NULL;
    }

    uint32_t group_length = dcm_element_get_value_UL(element, 0);
    eheader_destroy(header);
    dcm_element_destroy(element);

    // File Meta Information Version
    header = read_element_header(error, filehandle, &position, implicit);
    if (header == NULL) {
        dcm_dataset_destroy(filehandle_meta);
        return NULL;
    }

    element = read_element(error, filehandle, header, &position, implicit);
    if (element == NULL) {
        eheader_destroy(header);
        dcm_dataset_destroy(filehandle_meta);
        return NULL;
    }

    eheader_destroy(header);
    dcm_element_destroy(element);

    while(true) {
        header = read_element_header(error, filehandle, &position, implicit);
        if (header == NULL) {
            dcm_element_destroy(element);
            dcm_dataset_destroy(filehandle_meta);
            return NULL;
        }

        tag = eheader_get_tag(header);
        group_number = eheader_get_group_number(header);
        if (group_number != 0x0002) {
            eheader_destroy(header);
            break;
        }

        element = read_element(error, filehandle, header, &position, implicit);
        if (element == NULL) {
            eheader_destroy(header);
            dcm_dataset_destroy(filehandle_meta);
            return NULL;
        }

        if (!dcm_dataset_insert(error, filehandle_meta, element)) {
            dcm_error_set(error, DCM_ERROR_CODE_PARSE,
                          "Reading File Meta Information failed",
                          "Could not insert Data Element '%08X' into "
                          "Data Set", tag);
            eheader_destroy(header);
            dcm_dataset_destroy(filehandle_meta);
            return NULL;
        }

        if (position >= group_length) {
            eheader_destroy(header);
            break;
        }
        eheader_destroy(header);
    }

    if (!dcm_offset(error, filehandle, &filehandle->offset)) {
        dcm_dataset_destroy(filehandle_meta);
        return NULL;
    }

    element = dcm_dataset_get(error, filehandle_meta, 0x00020010);
    const char *transfer_syntax_uid = dcm_element_get_value_UI(element, 0);
    filehandle->transfer_syntax_uid = dcm_strdup(error, transfer_syntax_uid);
    if (filehandle->transfer_syntax_uid == NULL) {
        filehandle->offset = 0;  // reset state
        dcm_dataset_destroy(filehandle_meta);
        return NULL;
    }

    dcm_dataset_lock(filehandle_meta);

    return filehandle_meta;
}


DcmDataSet *dcm_filehandle_read_metadata(DcmError **error, DcmFilehandle *filehandle)
{
    uint32_t tag;
    uint16_t group_number;
    bool implicit;
    DcmElement *element;
    EHeader *header;

    if (filehandle->offset == 0) {
        DcmDataSet *filehandle_meta = 
            dcm_filehandle_read_filehandle_meta(error, filehandle);
        if (filehandle_meta == NULL) {
            return NULL;
        }

        dcm_dataset_destroy(filehandle_meta);
    }

    if (!dcm_seekset(error, filehandle, filehandle->offset)) {
        return NULL;
    }

    implicit = false;
    if (filehandle->transfer_syntax_uid) {
        if (strcmp(filehandle->transfer_syntax_uid, "1.2.840.10008.1.2") == 0) {
            implicit = true;
        }
    }

    DcmDataSet *dataset = dcm_dataset_create(error);
    if (dataset == NULL) {
        return NULL;
    }

    int64_t position = 0;
    for (;;) {
        if (dcm_is_eof(filehandle)) {
            dcm_log_info("Stop reading Data Set. Reached end of filehandle.");
            break;
        }

        header = read_element_header(error, filehandle, &position, implicit);
        if (header == NULL) {
            dcm_dataset_destroy(dataset);
            return NULL;
        }

        tag = eheader_get_tag(header);
        group_number = eheader_get_group_number(header);
        if (tag == TAG_TRAILING_PADDING) {
            dcm_log_info("Stop reading Data Set",
                          "Encountered Data Set Trailing Tag");
            eheader_destroy(header);
            break;
        } else if (tag == TAG_PIXEL_DATA ||
                   tag == TAG_FLOAT_PIXEL_DATA ||
                   tag == TAG_DOUBLE_PIXEL_DATA) {
            // Set filehandle pointer to the first byte of the 
            // pixel data element
            if (implicit) {
                // Tag: 4 bytes, Value Length: 4 bytes
                if (!dcm_seekcur(error, filehandle, -8, &position)) {
                    eheader_destroy(header);
                    dcm_dataset_destroy(dataset);
                    return NULL;
                }

            } else {
                // Tag: 4 bytes, VR: 2 bytes + 2 bytes, Value Length: 4 bytes
                if (!dcm_seekcur(error, filehandle, -12, &position)) {
                    eheader_destroy(header);
                    dcm_dataset_destroy(dataset);
                    return NULL;
                }
            }

            if (!dcm_offset(error, 
                            filehandle, &filehandle->pixel_data_offset)) {
                eheader_destroy(header);
                dcm_dataset_destroy(dataset);
                return NULL;
            }

            dcm_log_debug("Stop reading Data Set. "
                          "Encountered Tag of Pixel Data Element.");
            eheader_destroy(header);
            break;
        }
        if (group_number == 0x0002) {
            dcm_error_set(error, DCM_ERROR_CODE_PARSE,
                          "Reading of Data Set failed",
                          "Encountered File Meta Information group");
            eheader_destroy(header);
            dcm_dataset_destroy(dataset);
            return NULL;
        }

        element = read_element(error, filehandle, header, &position, implicit);
        if (element == NULL) {
            eheader_destroy(header);
            dcm_dataset_destroy(dataset);
            return NULL;
        }
        if (!dcm_dataset_insert(error, dataset, element)) {
            eheader_destroy(header);
            dcm_dataset_destroy(dataset);
            return NULL;
        }
        eheader_destroy(header);
    }
    dcm_dataset_lock(dataset);

    return dataset;
}


static bool get_num_frames(DcmError **error, 
                           const DcmDataSet *metadata,
                           uint32_t *number_of_frames)
{
    const uint32_t tag = 0x00280008;

    DcmElement *element = dcm_dataset_get(error, metadata, tag);
    if (element == NULL) {
        return false;
    }

    const char *value = dcm_element_get_value_IS(element, 0);
    *number_of_frames = (uint32_t) strtol(value, NULL, 10);

    return true;
}


DcmBOT *dcm_filehandle_read_bot(DcmError **error, DcmFilehandle *filehandle, DcmDataSet *metadata)
{
    uint64_t value;

    dcm_log_debug("Reading Basic Offset Table.");

    if (!dcm_is_encapsulated_transfer_syntax(filehandle->transfer_syntax_uid)) {
        dcm_error_set(error, DCM_ERROR_CODE_PARSE,
                      "Reading Basic Offset Table failed",
                      "Data Set with transfer syntax '%s' should not contain "
                      "a Basic Offset Table because it is not encapsulated",
                      filehandle->transfer_syntax_uid);
        return NULL;
    }

    uint32_t num_frames;
    if (!get_num_frames(error, metadata, &num_frames)) {
        return NULL;
    }
    if (num_frames == 0) {
        dcm_error_set(error, DCM_ERROR_CODE_PARSE,
                      "Reading Basic Offset Table failed",
                      "Value of Data Element 'Number of Frames' is "
                      "malformed");
        return NULL;
    }

    if (filehandle->pixel_data_offset == 0) {
        dcm_error_set(error, DCM_ERROR_CODE_PARSE,
                      "Reading Basic Offset Table failed",
                      "Could not determine offset of Pixel Data Element. "
                      "Read metadata first");
        return NULL;
    }

    if (!dcm_seekset(error, filehandle, filehandle->pixel_data_offset)) {
        return NULL;
    }

    // measure distance to first frame from pixel_data_offset
    int64_t position = 0;
    EHeader *eheader = read_element_header(error, filehandle, 
                                           &position, false);
    uint32_t eheader_tag = eheader_get_tag(eheader);
    eheader_destroy(eheader);
    if (!(eheader_tag == TAG_PIXEL_DATA ||
          eheader_tag == TAG_FLOAT_PIXEL_DATA ||
          eheader_tag == TAG_DOUBLE_PIXEL_DATA)) {
        dcm_error_set(error, DCM_ERROR_CODE_PARSE,
                      "Reading Basic Offset Table failed",
                      "File pointer not positioned at Pixel Data Element");
        return NULL;
    }

    // The header of the BOT Item
    IHeader *iheader = read_item_header(error, filehandle, &position);
    if (iheader == NULL) {
        iheader_destroy(iheader);
        return NULL;
    }
    uint32_t item_tag = iheader_get_tag(iheader);
    if (item_tag != TAG_ITEM) {
        dcm_error_set(error, DCM_ERROR_CODE_PARSE,
                      "Reading Basic Offset Table failed",
                      "Unexpected Tag found for Basic Offset Table Item");
        iheader_destroy(iheader);
        return NULL;
    }

    ssize_t *offsets = DCM_NEW_ARRAY(error, num_frames, ssize_t);
    if (offsets == NULL) {
        iheader_destroy(iheader);
        return NULL;
    }

    // The BOT Item must be present, but the value is optional
    uint32_t item_length = iheader_get_length(iheader);
    ssize_t first_frame_offset;
    iheader_destroy(iheader);
    if (item_length > 0) {
        dcm_log_info("Read Basic Offset Table value.");
        // Read offset values from BOT Item value
        for (uint32_t i = 0; i < num_frames; i++) {
            uint32_t ui32;
            if (!read_uint32(error, filehandle, &ui32, &position)) {
                free(offsets);
                return NULL;
            }
            value = (uint64_t)ui32;

            if (value == TAG_ITEM) {
                dcm_error_set(error, DCM_ERROR_CODE_PARSE,
                              "Reading Basic Offset Table failed",
                              "Encountered unexpected Item Tag "
                              "in Basic Offset Table");
                free(offsets);
                return NULL;
            }

            offsets[i] = value;
        }

        // and that's the offset to the item header on the first frame
        first_frame_offset = position;
    } else {
        dcm_log_info("Basic Offset Table is empty");
        // Handle Extended Offset Table attribute
        const DcmElement *eot_element = dcm_dataset_contains(metadata, 
                                                             0x7FE00001);
        if (eot_element) {
            dcm_log_info("Found Extended Offset Table.");
            const char *blob = dcm_element_get_value_OV(eot_element);
            for (uint32_t i = 0; i < num_frames; i++) {
                char *end_ptr;
                value = (uint64_t) strtoull(blob, &end_ptr, 64);
                // strtoull returns 0 in case of error
                // FIXME and also sets end_ptr to blob
                if (value == 0 && i > 0) {
                    dcm_error_set(error, DCM_ERROR_CODE_PARSE,
                                  "Reading Basic Offset Table failed",
                                  "Failed to parse value of Extended Offset "
                                  "Table element for frame #%d", i + 1);
                    free(offsets);
                    return NULL;
                }
                offsets[i] = value;
                blob = end_ptr;
            }
            // FIXME we should return the bot I guess?
        }
        return NULL;
    }

    return dcm_bot_create(error, offsets, num_frames, first_frame_offset);
}


static struct PixelDescription *create_pixel_description(DcmError **error,
    const DcmDataSet *metadata)
{
    DcmElement *element;

    struct PixelDescription *desc = DCM_NEW(error, struct PixelDescription);
    if (desc == NULL) {
        return NULL;
    }

    element = dcm_dataset_get(error, metadata, 0x00280010);
    if (element == NULL) {
        free(desc);
        return NULL;
    }
    desc->rows = dcm_element_get_value_US(element, 0);

    element = dcm_dataset_get(error, metadata, 0x00280011);
    if (element == NULL) {
        free(desc);
        return NULL;
    }
    desc->columns = dcm_element_get_value_US(element, 0);

    element = dcm_dataset_get(error, metadata, 0x00280002);
    if (element == NULL) {
        free(desc);
        return NULL;
    }
    desc->samples_per_pixel = dcm_element_get_value_US(element, 0);

    element = dcm_dataset_get(error, metadata, 0x00280100);
    if (element == NULL) {
        free(desc);
        return NULL;
    }
    desc->bits_allocated = dcm_element_get_value_US(element, 0);

    element = dcm_dataset_get(error, metadata, 0x00280101);
    if (element == NULL) {
        free(desc);
        return NULL;
    }
    desc->bits_stored = dcm_element_get_value_US(element, 0);

    element = dcm_dataset_get(error, metadata, 0x00280103);
    if (element == NULL) {
        free(desc);
        return NULL;
    }
    desc->pixel_representation = dcm_element_get_value_US(element, 0);

    element = dcm_dataset_get(error, metadata, 0x00280006);
    if (element == NULL) {
        free(desc);
        return NULL;
    }
    desc->planar_configuration = dcm_element_get_value_US(element, 0);

    element = dcm_dataset_get(error, metadata, 0x00280004);
    if (element == NULL) {
        free(desc);
        return NULL;
    }
    desc->photometric_interpretation = 
        dcm_strdup(error, dcm_element_get_value_CS(element, 0));
    if (desc->photometric_interpretation == NULL) {
        free(desc);
        return NULL;
    }

    return desc;
}


static void destroy_pixel_description(struct PixelDescription *desc)
{
    if (desc) {
        if (desc->photometric_interpretation) {
            free(desc->photometric_interpretation);
        }
        free(desc);
    }
}


DcmBOT *dcm_filehandle_build_bot(DcmError **error,
                                 DcmFilehandle *filehandle, 
                                 DcmDataSet *metadata)
{
    uint32_t item_tag, iheader_tag;
    uint32_t item_length;
    uint64_t i;
    IHeader *iheader;

    dcm_log_debug("Building Basic Offset Table.");

    uint32_t num_frames;
    if (!get_num_frames(error, metadata, &num_frames)) {
        return NULL;
    }
    if (num_frames == 0) {
        dcm_error_set(error, DCM_ERROR_CODE_PARSE,
                      "Building Basic Offset Table failed",
                      "Value of Data Element 'Number of Frames' is "
                      "malformed.");
        return NULL;
    }

    if (filehandle->pixel_data_offset == 0) {
        dcm_error_set(error, DCM_ERROR_CODE_PARSE,
                      "Reading Basic Offset Table failed",
                      "Could not determine offset of Pixel Data Element. "
                      "Read metadata first");
        return NULL;
    }

    if (!dcm_seekset(error, filehandle, filehandle->pixel_data_offset)) {
        return NULL;
    }

    // we measure offsets from this point
    int64_t position = 0;

    EHeader *eheader = read_element_header(error, 
                                           filehandle, &position, false);
    uint32_t eheader_tag = eheader_get_tag(eheader);
    eheader_destroy(eheader);
    if (eheader_tag != TAG_PIXEL_DATA &&
        eheader_tag != TAG_FLOAT_PIXEL_DATA &&
        eheader_tag != TAG_DOUBLE_PIXEL_DATA) {
        dcm_error_set(error, DCM_ERROR_CODE_PARSE,
                      "Building Basic Offset Table failed",
                      "Pixel data offset not positioned at Pixel Data Element");
        return NULL;
    }

    ssize_t *offsets = DCM_NEW_ARRAY(error, num_frames, ssize_t);
    if (offsets == NULL) {
        return NULL;
    }

    ssize_t first_frame_offset;

    if (dcm_is_encapsulated_transfer_syntax(filehandle->transfer_syntax_uid)) {
        // The header of the BOT Item
        iheader = read_item_header(error, filehandle, &position);
        if (iheader == NULL) {
            free(offsets);
            iheader_destroy(iheader);
            return NULL;
        }

        item_tag = iheader_get_tag(iheader);
        if (item_tag != TAG_ITEM) {
            dcm_error_set(error, DCM_ERROR_CODE_PARSE,
                          "Building Basic Offset Table failed",
                          "Unexpected Tag found for Basic Offset Table Item");
            free(offsets);
            iheader_destroy(iheader);
            return NULL;
        }

        // The BOT Item must be present, but the value is optional
        item_length = iheader_get_length(iheader);
        iheader_destroy(iheader);

        // Move filehandlepointer to the first byte of first Frame item
        if (!dcm_seekcur(error, filehandle, item_length, &position)) {
            free(offsets);
        }

        // and that's the offset to the first frame
        first_frame_offset = position;

        // now measure positions from the start of the first frame
        position = 0;

        i = 0;
        while (true) {
            iheader = read_item_header(error, filehandle, &position);
            if (iheader == NULL) {
                free(offsets);
                iheader_destroy(iheader);
                return NULL;
            }

            iheader_tag = iheader_get_tag(iheader);
            if (iheader_tag == TAG_SQ_DELIM) {
                break;
            }
            if (iheader_tag != TAG_ITEM) {
                dcm_error_set(error, DCM_ERROR_CODE_PARSE,
                              "Building Basic Offset Table failed",
                              "Frame Item #%d has wrong Tag '%08X'",
                              i + 1,
                              iheader_tag);
                free(offsets);
                iheader_destroy(iheader);
                return NULL;
            }
            if (dcm_is_eof(filehandle)) {
                break;
            }

            // step back to the start of the item for this frame
            offsets[i] = position - 8;

            item_length = iheader_get_length(iheader);
            iheader_destroy(iheader);
            if (!dcm_seekcur(error, filehandle, item_length, &position)) {
                free(offsets);
                return NULL;
            }

            i += 1;
        }

        if (i != num_frames) {
            dcm_error_set(error, DCM_ERROR_CODE_PARSE,
                          "Building Basic Offset Table failed",
                          "Found incorrect number of Frame Items");
            free(offsets);
            return NULL;
        }
    } else {
        struct PixelDescription *desc = create_pixel_description(error,
                                                                 metadata);
        if (desc == NULL) {
            free(offsets);
            destroy_pixel_description(desc);
            return NULL;
        }
        for (i = 0; i < num_frames; i++) {
            offsets[i] = i * 
                         desc->rows * 
                         desc->columns * 
                         desc->samples_per_pixel;
        }
        destroy_pixel_description(desc);

        // Header of Pixel Data Element
        first_frame_offset = 10;
    }

    return dcm_bot_create(error, offsets, num_frames, first_frame_offset);
}


DcmFrame *dcm_filehandle_read_frame(DcmError **error,
                                    DcmFilehandle *filehandle,
                                    DcmDataSet *metadata,
                                    DcmBOT *bot,
                                    uint32_t number)
{
    uint32_t length;

    dcm_log_debug("Read Frame Item #%d.", number);
    if (number == 0) {
        dcm_error_set(error, DCM_ERROR_CODE_PARSE,
                      "Reading Frame Item failed",
                      "Frame Number must be positive");
        return NULL;
    }

    ssize_t frame_offset = dcm_bot_get_frame_offset(bot, number);
    ssize_t total_frame_offset = filehandle->pixel_data_offset + frame_offset;
    if (!dcm_seekset(error, filehandle, total_frame_offset)) {
        return NULL;
    }

    struct PixelDescription *desc = create_pixel_description(error, metadata);
    if (desc == NULL) {
        return NULL;
    }

    int64_t position = 0;
    if (dcm_is_encapsulated_transfer_syntax(filehandle->transfer_syntax_uid)) {
        IHeader *iheader = read_item_header(error, filehandle, &position);
        if (iheader == NULL) {
            destroy_pixel_description(desc);
            return NULL;
        }
        uint32_t iheader_tag = iheader_get_tag(iheader);
        if (iheader_tag != TAG_ITEM) {
            dcm_error_set(error, DCM_ERROR_CODE_PARSE,
                          "Reading Frame Item failed",
                          "No Item Tag found for Frame Item #%d",
                          number);
            destroy_pixel_description(desc);
            iheader_destroy(iheader);
            return NULL;
        }
        length = iheader_get_length(iheader);
        iheader_destroy(iheader);
    } else {
        length = desc->rows * desc->columns * desc->samples_per_pixel;
    }

    char *value = DCM_MALLOC(error, length);
    if (value == NULL) {
        destroy_pixel_description(desc);
        return NULL;
    }
    if (!dcm_require(error, filehandle, value, length, &position)) {
        destroy_pixel_description(desc);
        free(value);
        return NULL;
    }

    char *transfer_syntax_uid = dcm_strdup(error, 
                                           filehandle->transfer_syntax_uid);
    if (transfer_syntax_uid == NULL) {
        destroy_pixel_description(desc);
        free(value);
        return NULL;
    }

    char *photometric_interpretation = 
        dcm_strdup(error, desc->photometric_interpretation);
    if (photometric_interpretation == NULL) {
        free(transfer_syntax_uid);
        free(value);
        destroy_pixel_description(desc);
        return NULL;
    }

    DcmFrame *frame = dcm_frame_create(error,
                                       number,
                                       value,
                                       length,
                                       desc->rows,
                                       desc->columns,
                                       desc->samples_per_pixel,
                                       desc->bits_allocated,
                                       desc->bits_stored,
                                       desc->pixel_representation,
                                       desc->planar_configuration,
                                       photometric_interpretation,
                                       transfer_syntax_uid);
    destroy_pixel_description(desc);

    return frame;
}
