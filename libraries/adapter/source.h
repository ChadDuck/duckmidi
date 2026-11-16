#ifndef DES_SOURCE_H
#define DES_SOURCE_H

#include "../common/common.h"
#include "../../vendor/yyjson-0.12.0/yyjson.h"

#ifdef __cplusplus
extern "C" {
#endif

bool es_make_value(const EsFile *, const char *, char out[64], bool *, char *, size_t);

typedef struct {
    size_t begin, end;
    char *data;
    size_t size, order;
} EsEdit;

typedef struct {
    EsEdit *items;
    size_t count, capacity;
} EsEdits;

typedef struct EsJson EsJson;
typedef struct EsXml EsXml;

typedef struct {
    const char *name, *value;
    size_t begin, end;
} EsXmlAttribute;

typedef struct {
    const char *name;
    const EsXmlAttribute *attributes;
    size_t attribute_count, parent, begin, header_end, close;
    bool empty;
} EsXmlNode;

bool es_edits_add(EsEdits *, size_t begin, size_t end, const char *, size_t, char *error,
                  size_t cap);
bool es_edits_apply(EsEdits *, const void *, size_t, EsFile *, char *, size_t);
void es_edits_free(EsEdits *);
EsJson *es_json_parse(const void *, size_t, char *, size_t);
yyjson_val *es_json_root(EsJson *);
/* Replaces one object member while retaining all other source bytes/comments. Value is JSON. */
bool es_json_set(EsJson *, yyjson_val *object, const char *member, const char *value, EsEdits *,
                 char *, size_t);
bool es_json_rename(EsJson *, yyjson_val *object, const char *member, const char *name, EsEdits *,
                    char *, size_t);
char *es_json_quote(const char *);
void es_json_free(EsJson *);
EsXml *es_xml_parse(const void *, size_t, char *, size_t);
size_t es_xml_count(const EsXml *);
const EsXmlNode *es_xml_node(const EsXml *, size_t index);
const char *es_xml_attribute(const EsXmlNode *, const char *name);
bool es_xml_set_attribute(EsXml *, size_t node, const char *name, const char *value, EsEdits *,
                          char *, size_t);
bool es_xml_append(EsXml *, size_t node, const char *fragment, EsEdits *, char *, size_t);
char *es_xml_escape(const char *);
void es_xml_free(EsXml *);

#ifdef __cplusplus
}
#endif
#endif
