/* SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright(c) 2010-2019 Kamiar Kanani
 */

#ifndef MULTIFAST_SERIALIZER_H
#define MULTIFAST_SERIALIZER_H

/**
 * @file
 * Multifast Serializer
 *
 * The serializer API provides serialization and de-serialization operations
 * for the trie.
 */

/* includes */

#ifdef __cplusplus
extern "C" {
#endif

struct serialize_curser {
    char *buffer;
    char *free_ptr;
    size_t buf_size;
    int retval;
    int simulate;
};

#ifdef __cplusplus
}
#endif

#endif /* MULTIFAST_SERIALIZER_H */
