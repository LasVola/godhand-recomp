// The part of libzip's API that PCSX2's GS uses (common/ZipHelpers.h, GSTextureReplacements.cpp), without libzip:
// texture packs in .zip files are not supported (every open fails, so only loose replacement files are used).
// PCSX2 is GPL-3; so is this file.
#pragma once

#include <cstddef>
#include <cstdint>

typedef int64_t zip_int64_t;
typedef uint64_t zip_uint64_t;
typedef uint32_t zip_flags_t;

struct zip_t;
struct zip_source_t;
struct zip_file_t;

struct zip_error_t
{
    int zip_err;
    int sys_err;
    char *str;
};

struct zip_stat_t
{
    zip_uint64_t valid;
    const char *name;
    zip_uint64_t index;
    zip_uint64_t size;
    zip_uint64_t comp_size;
};

#define ZIP_RDONLY 16
#define ZIP_FL_NOCASE 1u
#define ZIP_ER_COMPNOTSUPP 16

inline zip_source_t *zip_source_file_create(const char *, zip_uint64_t, zip_int64_t, zip_error_t *) { return nullptr; }
inline zip_source_t *zip_source_buffer_create(const void *, zip_uint64_t, int, zip_error_t *) { return nullptr; }
inline zip_t *zip_open_from_source(zip_source_t *, int, zip_error_t *) { return nullptr; }
inline void zip_source_free(zip_source_t *) {}
inline int zip_close(zip_t *) { return 0; }
inline void zip_discard(zip_t *) {}
inline zip_file_t *zip_fopen(zip_t *, const char *, zip_flags_t) { return nullptr; }
inline zip_file_t *zip_fopen_index(zip_t *, zip_uint64_t, zip_flags_t) { return nullptr; }
inline int zip_fclose(zip_file_t *) { return 0; }
inline zip_int64_t zip_fread(zip_file_t *, void *, zip_uint64_t) { return -1; }
inline zip_int64_t zip_name_locate(zip_t *, const char *, zip_flags_t) { return -1; }
inline int zip_stat_index(zip_t *, zip_uint64_t, zip_flags_t, zip_stat_t *) { return -1; }
inline zip_int64_t zip_get_num_entries(zip_t *, zip_flags_t) { return -1; }
inline const char *zip_get_name(zip_t *, zip_uint64_t, zip_flags_t) { return nullptr; }
inline const char *zip_error_strerror(zip_error_t *) { return "zip files are not supported"; }
inline const char *zip_strerror(zip_t *) { return "zip files are not supported"; }
inline zip_error_t *zip_get_error(zip_t *)
{
    static zip_error_t error = {};
    return &error;
}
