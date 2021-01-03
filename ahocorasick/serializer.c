/* SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright(c) 2010-2020 Kamiar Kanani
 */

#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "actypes.h"
#include "node.h"
#include "ahocorasick.h"
#include "serializer.h"

extern void ac_trie_traverse_action 
    (ACT_NODE_t *node, void(*func)(ACT_NODE_t *, void *), 
        int top_down, void *user);

void append_str(struct serialize_curser *cur, char *str, int len)
{
    if (cur->simulate) {
        cur->free_ptr += len;
        return;
    }
    if (cur->buf_size - (cur->free_ptr - cur->buffer) > len) {
        strncpy(cur->free_ptr, str, len);
        cur->free_ptr += len;
    } else {
        cur->retval = 1;
    }
}
#define append_const_str(cur,cstr) append_str(cur,cstr,sizeof(cstr)-1);

static char * mf_itoa(long int n, char *a, size_t size) {
    size_t i = 0, j, k;
    while (n && i < size - 2)
    {
        a[i++] = (char)(48 + (n % 10));
        n /= 10;
    }
    /* if number is 0 */
    if (i == 0)
        a[i++] = '0';
    a[i] = '\0';
    /* Reverse */
    for (j = 0; j < i / 2; j++) {
        k = a[j];
        a[j] = a[i - j - 1];
        a[i - j - 1] = k;
    }
    return a;
}

void serialize_id(struct serialize_curser *cur, AC_PATTID_t id)
{
    char buffer[40];
    if (id.type == AC_PATTID_TYPE_STRING) {
        append_const_str(cur, "'");
        append_str(cur, (char *)id.u.stringy, strlen(id.u.stringy));
        append_const_str(cur, "'");
    } else {
        mf_itoa(id.u.number, buffer, sizeof(buffer));
        append_str(cur, buffer, strlen(buffer));
    }
}

void serialize_node(ACT_NODE_t *nod, void *user)
{
    int len, i, j;
    char str[AC_PATTRN_MAX_LENGTH];
    struct serialize_curser *cur = (struct serialize_curser *)user;

    /* Serialize node id and failure node id */
    append_const_str(cur, "N = (");
    mf_itoa(nod->id, str, sizeof(str));
    append_str(cur, str, strlen(str));
    append_const_str(cur, ", ");
    j = nod->failure_node ? nod->failure_node->id : 0;
    mf_itoa(j, str, sizeof(str));
    append_str(cur, str, strlen(str));

    /* Serialize edges */
    append_const_str(cur, ", {");
    for (i = 0; i < nod->outgoing_size; i++) {
        if (i) append_const_str(cur, ", ");
        mf_itoa((unsigned)nod->outgoing[i].alpha, str, sizeof(str));
        append_str(cur, str, strlen(str));
        append_const_str(cur, ":");
        mf_itoa(nod->outgoing[i].next->id, str, sizeof(str));
        append_str(cur, str, strlen(str));
    }
    append_const_str(cur, "}, [");

    /* Serialize matched pattern ids */
    for (i = 0; i < nod->matched_size; i++) {
        if (i) append_const_str(cur, ", ");
        serialize_id(cur, nod->matched[i].id);
    }
    append_const_str(cur, "], ");

    /* Find the longest matched pattern index */
    len = j = 0;
    for (i = 0; i < nod->matched_size; i++) {
        if (len < nod->matched[i].ptext.length) {
            len = nod->matched[i].ptext.length;
            j = i;
        }
    }

    /* Serialize pattern if there is any */
    append_const_str(cur, "[");
    if (len > 0) {
        if (nod->matched[j].rtext.astring) {
            for (i = 0; i < nod->matched[j].rtext.length; i++) {
                if (i) append_const_str(cur, ", ");
                mf_itoa((unsigned)nod->matched[j].rtext.astring[i], str, sizeof(str));
                append_str(cur, str, strlen(str));
            }
        }
    }
    append_const_str(cur, "])\n");
}

static int mf_serialized_size(AC_TRIE_t *thiz)
{
    struct serialize_curser cur;
    cur.buf_size = 0;
    cur.buffer = 0;
    cur.free_ptr = 0;
    cur.retval = 0;
    cur.simulate = 1;
    ac_trie_traverse_action(thiz->root, serialize_node, 1, &cur);
    
    return (cur.free_ptr - cur.buffer);
}

static void serialize_attributes(AC_TRIE_t *thiz, struct serialize_curser *cur)
{
    char buffer[40];
    (void)(thiz);
    append_const_str(cur, "ALPHA_SIZE = ");
    mf_itoa(sizeof(AC_ALPHABET_t), buffer, sizeof(buffer));
    append_str(cur, buffer, strlen(buffer));
    append_const_str(cur, "\n");

    append_const_str(cur, "PATTERN_ID_TYPE = ");
    /* TODO: use unique pattern id type for all */
    mf_itoa(AC_PATTID_TYPE_NUMBER, buffer, sizeof(buffer));
    append_str(cur, buffer, strlen(buffer));
    append_const_str(cur, "\n");
}

AC_STATUS_t mf_serialize(AC_TRIE_t *thiz, char **bytes, size_t *length)
{
    struct serialize_curser cur;
    cur.buf_size = ((mf_serialized_size(thiz) / 1024) + 1) * 1024;
    cur.buffer = (char *)malloc(cur.buf_size);
    cur.free_ptr = cur.buffer;
    cur.retval = 0;
    cur.simulate = 0;

    serialize_attributes(thiz, &cur);
    ac_trie_traverse_action(thiz->root, serialize_node, 1, &cur);

    *bytes = cur.buffer;
    *length = cur.free_ptr - cur.buffer;

    if (cur.retval) {
        free(cur.buffer);
        return ACERR_FAILURE;
    }
    return ACERR_SUCCESS;
}

enum token_type {
    TOKEN_UNKNOWN = 0,
    TOKEN_WORD,
    TOKEN_EQUAL,
    TOKEN_NUMBER,
    TOKEN_DELIMITER,
    TOKEN_BRAKET_OPEN,
    TOKEN_BRAKET_CLOSE,
    TOKEN_COLON,
    TOKEN_APOSTROPHE,
    TOKEN_STRING,
    TOKEN_EOF
};

static void get_next_token(char *in, size_t in_size,
        char **tok, size_t *tok_size, enum token_type *tok_type)
{
    int i = 0, begin = i;
    while (i < in_size && isspace(in[i])) i++;
    if (i >= in_size) {
        *tok_type = TOKEN_EOF;
        *tok = &in[begin];
        *tok_size = 0;
        return;
    }
    begin = i;
    if (isdigit(in[i])) {
        while (i < in_size && isdigit(in[i])) i++;
        *tok_type = TOKEN_NUMBER;
    } else if (isalpha(in[i])) {
        while (i < in_size &&
                (isalpha(in[i]) || isdigit(in[i]) || in[i] == '_')) i++;
        *tok_type = TOKEN_WORD;
    } else if (in[i] == '=') {
        i++;
        *tok_type = TOKEN_EQUAL;
    } else if (in[i] == ',') {
        i++;
        *tok_type = TOKEN_DELIMITER;
    } else if (in[i] == ':') {
        i++;
        *tok_type = TOKEN_COLON;
    } else if (in[i] == '\'') {
        i++;
        while (i < in_size && in[i] != '\'' &&
                (isdigit(in[i]) || isalpha(in[i]))) i++;
        if (i < in_size && in[i] == '\'') {
            i++;
            *tok_type = TOKEN_STRING;
        } else {
            *tok_type = TOKEN_UNKNOWN;
        }
    } else if (in[i] == '[' || in[i] == '{' || in[i] == '(') {
        i++;
        *tok_type = TOKEN_BRAKET_OPEN;
    } else if (in[i] == ']' || in[i] == '}' || in[i] == ')') {
        i++;
        *tok_type = TOKEN_BRAKET_CLOSE;
    } else {
        *tok_type = TOKEN_UNKNOWN;
    }
    *tok = &in[begin];
    *tok_size = i - begin;
}

enum keyword_id {
    KEYWORD_ID_INVALID = 0,
    KEYWORD_ID_ALPHA_SIZE,
    KEYWORD_ID_PATTERN_ID_TYPE,
    KEYWORD_ID_N,   /* NODE */
    KEYWORD_ID_MAX
};

char *get_keyword_by_id(enum keyword_id kid) {
    switch (kid) {
        case KEYWORD_ID_ALPHA_SIZE:
            return "ALPHA_SIZE";
        case KEYWORD_ID_PATTERN_ID_TYPE:
            return "PATTERN_ID_TYPE";
        case KEYWORD_ID_N:
            return "N";
        default:
            return "ERROR";
    }
}

enum keyword_id get_word_id(char *tok, size_t tok_size) {
    enum keyword_id i;
    char *word;
    int len;
    for (i = KEYWORD_ID_INVALID + 1; i < KEYWORD_ID_MAX; i++) {
        word = get_keyword_by_id(i);
        len = strlen(word);
        if (len != tok_size)
            continue;
        if (strncmp(tok, word, tok_size) == 0)
            return i;
    }
    return KEYWORD_ID_INVALID;
}

static int mf_atoi(char *ascii, size_t size, int *value) {
    int i;
    *value = 0;
    for (i = 0; i < size; i++) {
        if (ascii[i] >= '0' &&  ascii[i] <= '9') {
            *value *= 10;
            *value += (ascii[i] - 48);
        } else {
            return -1;
        }
    }
    return 0;
}

static int check_next_token(enum token_type tok_type,
        char **bytes, size_t *length,
        char **token, size_t *token_size, enum token_type *token_type) {
    get_next_token(*bytes, *length, token, token_size, token_type);
    if (*token_type != tok_type) {
        return 1;
    }
    *length -= ((*token + *token_size) - *bytes);
    *bytes = *token + *token_size;
    return 0;
}

/******************************************************************************/

struct trie_builder {
    AC_TRIE_t *trie;
    ACT_NODE_t **nodes;
    size_t size;
    int idx;    /* last empty room */
};

struct trie_builder *
trie_builder_init(void)
{
    struct trie_builder *tb = (struct trie_builder *)
        malloc(sizeof(struct trie_builder));
    tb->idx = 0;
    tb->size = 1024;
    tb->nodes = (ACT_NODE_t **)calloc(sizeof(ACT_NODE_t *), tb->size);
    tb->trie = NULL;

    return tb;
}

void 
trie_builder_free(struct trie_builder *tb)
{
    free(tb->nodes);
    free(tb);
}

struct act_node *
trie_builder_add_node(struct trie_builder *tb, int nod_id)
{
    struct act_node *node;
    if (nod_id - 1 != tb->idx) {
        printf("IDs must start from one and be incremental\n");
        return NULL;
    }
    if (tb->nodes[tb->idx]) {
        /* The node is already created by add_correction */
        return tb->nodes[tb->idx++];
    }
    if (tb->idx == 0) {
        tb->trie = ac_trie_create();
        node = tb->trie->root;
    } else {
        node = node_create(tb->trie);
    }
    node->id = nod_id;
    
    /* TODO: check index length */
    tb->nodes[tb->idx++] = node;

    return node;
}

int
trie_builder_add_correction(struct trie_builder *tb, ACT_NODE_t * node,
        int corr_id)
{
    ACT_NODE_t *new_node;

    if (corr_id == 0) {
        /* assert node->id == 1 */
        node->failure_node = NULL;
        return 0;
    }
    /* TODO: check corr id range */
    if (tb->nodes[corr_id - 1] == NULL) {
        new_node = node_create(tb->trie);
        new_node->id = corr_id;
        tb->nodes[corr_id - 1] = new_node;
    }
    node->failure_node = tb->nodes[corr_id - 1];

    return 0;
}

int
trie_builder_add_edge(struct trie_builder *tb, ACT_NODE_t *node, int alpha,
        int target_node)
{
    ACT_NODE_t *new_node;

    /* TODO: check target node range */
    if (tb->nodes[target_node - 1] == NULL) {
        new_node = node_create(tb->trie);
        new_node->id = target_node;
        tb->nodes[target_node - 1] = new_node;
    }
    node_add_edge(node, tb->nodes[target_node - 1], (AC_ALPHABET_t)alpha);

    return 0;
}

/******************************************************************************/

AC_STATUS_t mf_deserialize(AC_TRIE_t **triep, char *bytes, size_t length)
{
    char *token;
    size_t token_size;
    enum token_type token_type;

    #define PARAMS &bytes, &length, &token, &token_size, &token_type

    enum keyword_id wordid;
    int i, j, attrib = 0, attrib2 = 0;
    int guard = 1;
    /* TODO: Save ptype in the trie */
    enum ac_pattid_type ptype = AC_PATTID_TYPE_DEFAULT;
    ACT_NODE_t *node = NULL;
    struct trie_builder *tb = trie_builder_init();

    while (1) {
        if (check_next_token(TOKEN_WORD, PARAMS)) {
            if (token_type == TOKEN_EOF) {
                break;
            } else {
                goto failed;
            }
        }

        switch(wordid = get_word_id(token, token_size)) {
            case KEYWORD_ID_ALPHA_SIZE:
                if (check_next_token(TOKEN_EQUAL, PARAMS)) goto failed;
                if (check_next_token(TOKEN_NUMBER, PARAMS)) goto failed;
                if (mf_atoi(token, token_size, &attrib)) goto failed;
                if (attrib != sizeof(AC_ALPHABET_t)) {
                    goto failed;
                }
                break;

            case KEYWORD_ID_PATTERN_ID_TYPE:
                if (check_next_token(TOKEN_EQUAL, PARAMS)) goto failed;
                if (check_next_token(TOKEN_NUMBER, PARAMS)) goto failed;
                if (mf_atoi(token, token_size, &attrib)) goto failed;
                if (attrib == 1) {
                    ptype = AC_PATTID_TYPE_NUMBER;
                } else if (attrib == 2) {
                    ptype = AC_PATTID_TYPE_STRING;
                }
                /* TODO */
                printf("ID type is %d\n", ptype);
                break;

            case KEYWORD_ID_N:
                if (check_next_token(TOKEN_EQUAL, PARAMS)) goto failed;
                if (check_next_token(TOKEN_BRAKET_OPEN, PARAMS)) goto failed;
                if (check_next_token(TOKEN_NUMBER, PARAMS)) goto failed;
                if (mf_atoi(token, token_size, &attrib)) goto failed;
                if (!(node = trie_builder_add_node(tb, attrib))) {
                    goto failed;
                }
                printf("Node ID: %d\n", node->id);
                if (check_next_token(TOKEN_DELIMITER, PARAMS)) goto failed;
                if (check_next_token(TOKEN_NUMBER, PARAMS)) goto failed;
                if (mf_atoi(token, token_size, &attrib)) goto failed;
                if (trie_builder_add_correction(tb, node, attrib)) {
                    goto failed;
                }
                printf("Correction: %d\n", attrib);
                if (check_next_token(TOKEN_DELIMITER, PARAMS)) goto failed;
                if (check_next_token(TOKEN_BRAKET_OPEN, PARAMS)) goto failed;
                guard = 1;
                do {
                    if (!check_next_token(TOKEN_NUMBER, PARAMS)) {
                        if (mf_atoi(token, token_size, &attrib)) goto failed;
                        printf("Edge: %d -> ", attrib);
                        if (check_next_token(TOKEN_COLON, PARAMS)) goto failed;
                        if (check_next_token(TOKEN_NUMBER, PARAMS)) goto failed;
                        if (mf_atoi(token, token_size, &attrib2)) goto failed;
                        printf("%d\n", attrib2);
                        if (trie_builder_add_edge(tb, node, attrib, attrib2)) {
                            goto failed;
                        }
                    } else if (!check_next_token(TOKEN_BRAKET_CLOSE, PARAMS)) {
                        guard = 0;
                    } else if (check_next_token(TOKEN_DELIMITER, PARAMS)) {
                        goto failed;
                    }
                } while (guard);
                if (check_next_token(TOKEN_DELIMITER, PARAMS)) goto failed;
                if (check_next_token(TOKEN_BRAKET_OPEN, PARAMS)) goto failed;
                guard = 1;
                printf("Accepts = [");
                do {
                    if (!check_next_token(TOKEN_STRING, PARAMS)) {
                        printf("%.*s, ", (int)(token_size - 2), token + 1);
                    } else if (!check_next_token(TOKEN_NUMBER, PARAMS)) {
                        if (mf_atoi(token, token_size, &attrib)) goto failed;
                        printf("%d, ", attrib);
                    } else if (!check_next_token(TOKEN_BRAKET_CLOSE, PARAMS)) {
                        guard = 0;
                    } else if (check_next_token(TOKEN_DELIMITER, PARAMS)) {
                        goto failed;
                    }
                } while (guard);
                printf("]\n");
                if (check_next_token(TOKEN_DELIMITER, PARAMS)) goto failed;
                if (check_next_token(TOKEN_BRAKET_OPEN, PARAMS)) goto failed;
                guard = 1;
                printf("R = [");
                do {
                    if (!check_next_token(TOKEN_NUMBER, PARAMS)) {
                        if (mf_atoi(token, token_size, &attrib)) goto failed;
                        printf("%d, ", attrib);
                    } else if (!check_next_token(TOKEN_BRAKET_CLOSE, PARAMS)) {
                        guard = 0;
                    } else if (check_next_token(TOKEN_DELIMITER, PARAMS)) {
                        goto failed;
                    }
                } while (guard);
                printf("]\n");
                if (check_next_token(TOKEN_BRAKET_CLOSE, PARAMS)) goto failed;
                break;

            default:
                goto failed;
        }
        printf("\n");
    }

    printf("Node ids:\n");
    for (i = 0; i < tb->idx; i++) {
        node = tb->nodes[i];
        printf("%d(%d)\n", node->id, 
            node->failure_node ? node->failure_node->id : 0);
        for (j = 0; j < node->outgoing_size; j++) {
            printf("Edge: %d -> %d\n", 
                (unsigned)node->outgoing[j].alpha, node->outgoing[j].next->id);
        }
        printf("\n");
    }
    printf("\n");
    *triep = tb->trie;
    trie_builder_free(tb);
    return ACERR_SUCCESS;

failed:
    ac_trie_release(tb->trie);
    trie_builder_free(tb);
    return ACERR_FAILURE;
}
