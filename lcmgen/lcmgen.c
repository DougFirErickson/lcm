#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <inttypes.h>
#include <ctype.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "lcmgen.h"
#include "tokenize.h"

/** The LCM grammar is implemented here with a recursive-descent parser.
    handle_file is the top-level function, which calls parse_struct/parse_enum,
    and so on. 

    Every LCM type has an associated "signature", which is a hash of
    various components of its delcaration. If the declaration of a
    signature changes, the hash changes with high probability.

    Note that this implementation is sloppy about memory allocation:
    we don't worry about freeing memory since the program will exit
    after parsing anyway.
**/

lcm_struct_t *parse_struct(lcmgen_t *lcm, const char *lcmfile, tokenize_t *t);
lcm_enum_t *parse_enum(lcmgen_t *lcm, const char *lcmfile, tokenize_t *t);

// lcm's built-in types. Note that unsigned types are not present
// because there is no safe java implementation. Really, you don't
// want to add unsigned types.
static const char *primitive_types[] = { "int8_t",
                                         "int16_t",
                                         "int32_t",
                                         "int64_t",
                                         "byte",
                                         "float",
                                         "double",
                                         "string",
                                         "boolean",
                                         NULL};

// which types can be legally used as array dimensions?
static const char *array_dimension_types[] = { "int8_t",
                                               "int16_t",
                                               "int32_t",
                                               "int64_t",
                                               NULL};

// Given NULL-terminated array of strings "ts", does "t" appear in it?
static int string_in_array(const char *t, const char **ts)
{
    for (int i = 0; ts[i] != NULL; i++) {
        if (!strcmp(ts[i], t))
            return 1;
    }

    return 0;
}

int lcm_is_primitive_type(const char *t) 
{
    return string_in_array(t, primitive_types);
}

int lcm_is_array_dimension_type(const char *t)
{
    return string_in_array(t, array_dimension_types);
}

int lcm_is_legal_member_name(const char *t)
{
    return isalpha(t[0]) || t[0]=='_';
}

// Make the hash dependent on the value of the given character. The
// order that hash_update is called in IS important.
static int64_t hash_update(int64_t v, char c)
{
    v = ((v<<8) ^ (v>>55)) + c;

    return v;
}

// Make the hash dependent on each character in a string.
static int64_t hash_string_update(int64_t v, const char *s)
{
    v = hash_update(v, strlen(s));

    for (; *s != 0; s++)
        v = hash_update(v, *s);

    return v;
}
 
// Create a parsing context
lcmgen_t *lcmgen_create()
{
    lcmgen_t *lcmgen = (lcmgen_t*) calloc(1, sizeof(lcmgen_t));
    lcmgen->structs = g_ptr_array_new();
    lcmgen->enums = g_ptr_array_new();
    return lcmgen;
}

lcm_typename_t *lcm_typename_create(const char *typename)
{
    lcm_typename_t *lt = (lcm_typename_t*) calloc(1, sizeof(lcm_typename_t));

    lt->typename = strdup(typename);

    // package name: everything before the last ".", or "" if there is no "."
    //
    // shortname: everything after the last ".", or everything if
    // there is no "."
    //
    char *tmp = strdup(typename);
    char *rtmp = strrchr(tmp, '.');
    if (rtmp == NULL) {
        lt->package = ""; 
        lt->shortname = tmp;
    } else {
        lt->package = tmp;
        *rtmp = 0;
        lt->shortname = &rtmp[1];
    }

    return lt;
}

lcm_struct_t *lcm_struct_create(const char *lcmfile, const char *structname)
{
    lcm_struct_t *lr = (lcm_struct_t*) calloc(1, sizeof(lcm_struct_t));
    lr->lcmfile    = strdup(lcmfile);
    lr->structname = lcm_typename_create(structname);
    lr->members    = g_ptr_array_new();
    lr->enums      = g_ptr_array_new();
    lr->structs    = g_ptr_array_new();
    return lr;
}

lcm_enum_t *lcm_enum_create(const char *lcmfile, const char *name)
{
    lcm_enum_t *le = (lcm_enum_t*) calloc(1, sizeof(lcm_enum_t));
    le->lcmfile  = strdup(lcmfile);
    le->enumname = lcm_typename_create(name);
    le->values   = g_ptr_array_new();

    return le;
}

lcm_enum_value_t *lcm_enum_value_create(const char *name)
{
    lcm_enum_value_t *lev = (lcm_enum_value_t*) calloc(1, sizeof(lcm_enum_t));

    lev->valuename = strdup(name);

    return lev;
}

lcm_member_t *lcm_member_create()
{
    lcm_member_t *lm = (lcm_member_t*) calloc(1, sizeof(lcm_member_t));
    lm->dimensions = g_ptr_array_new();
    return lm;
}

int64_t lcm_struct_hash(lcm_struct_t *lr)
{
    int64_t v = 0x12345678;

    // NO: Purposefully, we do NOT include the structname in the hash.
    // this allows people to rename data types and still have them work.
    // 
    // In contrast, we DO hash the types of a structs members (and their names).
    //  v = hash_string_update(v, lr->structname);

    for (unsigned int i = 0; i < g_ptr_array_size(lr->members); i++) {
        lcm_member_t *lm = g_ptr_array_index(lr->members, i);

        // hash the member name
        v = hash_string_update(v, lm->membername);

        // if the member is a primitive type, include the type
        // signature in the hash. Do not include them for compound
        // members, because their contents will be included, and we
        // don't want a struct's name change to break the hash.
        if (lcm_is_primitive_type(lm->type->typename))
            v = hash_string_update(v, lm->type->typename);

        // hash the dimensionality information
        int ndim = g_ptr_array_size(lm->dimensions);
        v = hash_update(v, ndim);
        for (int j = 0; j < ndim; j++) {
            lcm_dimension_t *dim = (lcm_dimension_t*) g_ptr_array_index(lm->dimensions, j);
            v = hash_update(v, dim->mode);
            v = hash_string_update(v, dim->size);
        }
    }

    return v;
}

// The hash for LCM enums is defined only by the name of the enum;
// this allows bit declarations to be added over time.
int64_t lcm_enum_hash(lcm_enum_t *le)
{
    int64_t v = 0x87654321;

    v = hash_string_update(v, le->enumname->typename);
    return v;
}

// semantic error: it parsed fine, but it's illegal. (we don't try to
// identify the offending token). This function does not return.
void semantic_error(tokenize_t *t, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);

    printf("\n");
    vprintf(fmt, ap);
    printf("\n");

    printf("%s : %i\n", t->path, t->line);
    printf("%s", t->line_buffer);

    va_end(ap);
    _exit(0);
}

// semantic warning: it parsed fine, but it's dangerous.
void semantic_warning(tokenize_t *t, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);

    printf("\n");
    vprintf(fmt, ap);
    printf("\n");

    printf("%s : %i\n", t->path, t->line);
    printf("%s", t->line_buffer);

    va_end(ap);
}

// parsing error: we cannot continue. This function does not return.
void parse_error(tokenize_t *t, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);

    printf("\n");
    vprintf(fmt, ap);
    printf("\n");

    printf("%s : %i\n", t->path, t->line);
    printf("%s", t->line_buffer);
    for (int i = 0; i < t->column; i++) {
        if (isspace(t->line_buffer[i]))
            printf("%c", t->line_buffer[i]);
        else
            printf(" ");
    }
    printf("^\n");

    va_end(ap);
    _exit(0);
}

// If the next token is "tok", consume it and return 1. Else, return 0
int parse_try_consume(tokenize_t *t, const char *tok)
{
    int res = tokenize_peek(t);
    if (res == EOF)
        parse_error(t, "End of file while looking for %s.", tok);

    res = (!strcmp(t->token, tok));

    // consume if the token matched
    if (res)
        tokenize_next(t);

    return res;
}

// Consume the next token. If it's not "tok", an error is emitted and
// the program exits.
void parse_require(tokenize_t *t, char *tok)
{
    int res = tokenize_next(t);
    if (res == EOF || strcmp(t->token, tok)) 
        parse_error(t, "expected token %s", tok);

}

// require that the next token exist (not EOF). Description is a
// human-readable description of what was expected to be read. 
void require_next(tokenize_t *t, const char *description)
{
    int res = tokenize_next(t);
    if (res == EOF)
        parse_error(t, "End of file reached, expected %s.", description);
}

// parse a member declaration. This looks long and scary, but most of
// the code is for semantic analysis (error checking)
int parse_member(lcmgen_t *lcmgen, lcm_struct_t *lr, tokenize_t *t)
{
    lcm_typename_t *lt = NULL;

    // First, read a type specification. Then read members (multiple
    // members can be defined per-line.) Each member can have
    // different array dimensionalities.

    // inline type declaration?
    if (parse_try_consume(t, "struct")) {
        parse_error(t, "recursive structs not implemented.");
    } else if (parse_try_consume(t, "enum")) {
        parse_error(t, "recursive enums not implemented.");
    } else if (parse_try_consume(t, "union")) {
        parse_error(t, "recursive unions not implemented.");
    }

    // standard declaration
    require_next(t, "type identifier");
    
    if (!isalpha(t->token[0]) && t->token[0]!='_')
        parse_error(t, "invalid type name");
    
    lt = lcm_typename_create(t->token);

    while (1) {

        // get the lcm type name
        require_next(t, "name identifier");

        if (!lcm_is_legal_member_name(t->token))
            parse_error(t, "Invalid member name: must start with [a-zA-Z_].");

        // make sure this name isn't already taken.
        if (lcm_find_member(lr, t->token) != NULL)
            semantic_error(t, "Duplicate member name '%s'.", t->token);

        // create a new member
        lcm_member_t *lm = lcm_member_create();
        lm->type = lt;
        lm->membername = strdup(t->token);
        g_ptr_array_add(lr->members, lm);

        // (multi-dimensional) array declaration?
        while (parse_try_consume(t, "[")) {

            // pull out the size of the dimension, either a number or a variable name.
            require_next(t, "array size");

            lcm_dimension_t *dim = (lcm_dimension_t*) calloc(1, sizeof(lcm_dimension_t));
            
            if (isdigit(t->token[0])) {
                // we have a constant size array declaration.
                int sz = strtol(t->token, NULL, 0);
                if (sz <= 0)
                    semantic_error(t, "Constant array size must be > 0");

                dim->mode = LCM_CONST;
                dim->size = strdup(t->token);

            } else {
                // we have a variable sized declaration.
                if (t->token[0]==']')
                    semantic_error(t, "Array sizes must be declared either as a constant or variable.");
                if (!lcm_is_legal_member_name(t->token))
                    semantic_error(t, "Invalid array size variable name: must start with [a-zA-Z_].");

                // make sure the named variable is 
                // 1) previously declared and 
                // 2) an integer type
                int okay = 0;

                for (unsigned int i = 0; i < g_ptr_array_size(lr->members); i++) {
                    lcm_member_t *thislm = (lcm_member_t*) g_ptr_array_index(lr->members, i);
                    if (!strcmp(thislm->membername, t->token)) {
                        if (g_ptr_array_size(thislm->dimensions) != 0)
                            semantic_error(t, "Array dimension '%s' must be not be an array type.", t->token);
                        if (!lcm_is_array_dimension_type(thislm->type->typename))
                            semantic_error(t, "Array dimension '%s' must be an integer type.", t->token);
                        okay = 1;
                        break;
                    }
                }
                if (!okay) 
                    semantic_error(t, "Unknown variable array index '%s'. Index variables must be declared before the array.", t->token);

                dim->mode = LCM_VAR;
                dim->size = strdup(t->token);
            }
            parse_require(t, "]");

            // increase the dimensionality of the array by one dimension.
            g_ptr_array_add(lm->dimensions, dim);
        }

        if (!parse_try_consume(t, ",")) 
            break;
    }

    parse_require(t, ";");

    return 0;
}

int parse_enum_value(lcm_enum_t *le, tokenize_t *t)
{
    require_next(t, "enum name");

    lcm_enum_value_t *lev = lcm_enum_value_create(t->token);

    if (parse_try_consume(t, "=")) {
        require_next(t, "enum value literal");

        lev->value = strtol(t->token, NULL, 0);
    } else {
        // the didn't specify the value, compute the next largest value
        int32_t max = 0;

        for (unsigned int i = 0; i < g_ptr_array_size(le->values); i++) {
            lcm_enum_value_t *tmp = g_ptr_array_index(le->values, i);
            if (tmp->value > max)
                max = tmp->value;
        }

        lev->value = max + 1;
    }

    // make sure there aren't any duplicate values
    for (unsigned int i = 0; i < g_ptr_array_size(le->values); i++) {
        lcm_enum_value_t *tmp = g_ptr_array_index(le->values, i);
        if (tmp->value == lev->value)
            semantic_error(t, "Enum values %s and %s have the same value %d!", tmp->valuename, lev->valuename, lev->value);
        if (!strcmp(tmp->valuename, lev->valuename))
            semantic_error(t, "Enum value %s declared twice!", tmp->valuename);
    }

    g_ptr_array_add(le->values, lev);
    return 0;
}

/** assume the "struct" token is already consumed **/
lcm_struct_t *parse_struct(lcmgen_t *lcmgen, const char *lcmfile, tokenize_t *t)
{
    char     *name;

    require_next(t, "struct name");
    name = strdup(t->token);

    lcm_struct_t *lr = lcm_struct_create(lcmfile, name);
    
    parse_require(t, "{");
    
    while (!parse_try_consume(t, "}"))
        parse_member(lcmgen, lr, t);
    
    lr->hash = lcm_struct_hash(lr);

    free(name);
    return lr;
}

/** assumes the "enum" token is already consumed **/
lcm_enum_t *parse_enum(lcmgen_t *lcmgen, const char *lcmfile, tokenize_t *t)
{
    char     *name;

    require_next(t, "enum name");
    name = strdup(t->token);

    lcm_enum_t *le = lcm_enum_create(lcmfile, name);
    parse_require(t, "{");
    
    while (!parse_try_consume(t, "}")) {
        parse_enum_value(le, t);
        
        parse_try_consume(t, ",");
        parse_try_consume(t, ";");
    }

    le->hash = lcm_enum_hash(le);
    free(name);
    return le;
}

/** parse entity (top-level construct), return EOF if eof. **/
int parse_entity(lcmgen_t *lcmgen, const char *lcmfile, tokenize_t *t)
{
    int res;

    res = tokenize_next(t);
    if (res==EOF)
        return EOF;

    if (!strcmp(t->token, "struct")) {
        lcm_struct_t *lr = parse_struct(lcmgen, lcmfile, t);
        g_ptr_array_add(lcmgen->structs, lr);
        return 0;
    }

    if (!strcmp(t->token, "enum")) {
        lcm_enum_t *le = parse_enum(lcmgen, lcmfile, t);
        g_ptr_array_add(lcmgen->enums, le);
        return 0;
    }

    if (!strcmp(t->token, "union")) {
        parse_error(t,"unions not implemented\n");

        return 0;
    }

    parse_error(t,"Missing struct/enum/union token.");
    return -1;

}

int lcmgen_handle_file(lcmgen_t *lcmgen, const char *path)
{
    tokenize_t *t = tokenize_create(path);

    if (t==NULL) {
        perror(path);
        return -1;
    }

    if (getopt_get_bool(lcmgen->gopt, "tokenize")) {
        int ntok = 0;
        printf("%6s %6s %6s: %s\n", "tok#", "line", "col", "token");

        while (tokenize_next(t)!=EOF)
            printf("%6i %6i %6i: %s\n", ntok++, t->line, t->column, t->token);
        return 0;
    }

    int res;
    do {
        res = parse_entity(lcmgen, path, t);
    } while (res != EOF);

    tokenize_destroy(t);
    return 0;
}

void lcm_typename_dump(lcm_typename_t *lt)
{
    char buf[1024];
    int pos = 0;

    pos += sprintf(&buf[pos], "%s", lt->typename);

    printf("\t%-20s", buf);
}

void lcm_member_dump(lcm_member_t *lm)
{
    lcm_typename_dump(lm->type);

    printf("  ");

    printf("%s", lm->membername);

    int ndim = g_ptr_array_size(lm->dimensions);
    for (int i = 0; i < ndim; i++) {
        lcm_dimension_t *dim = g_ptr_array_index(lm->dimensions, i);
        switch (dim->mode) 
        {
        case LCM_CONST:
            printf(" [ (const) %s ]", dim->size);
            break;
        case LCM_VAR:
            printf(" [ (var) %s ]", dim->size);
            break;
        default:
            // oops! unhandled case
            assert(0);
        }
    }

    printf("\n");
}

void lcm_enum_dump(lcm_enum_t *le)
{
    printf("enum %s\n", le->enumname->typename);
    for (unsigned int i = 0; i < g_ptr_array_size(le->values); i++) {
        lcm_enum_value_t *lev = g_ptr_array_index(le->values, i);
        printf("        %-20s  %i\n", lev->valuename, lev->value);
    }
}

void lcm_struct_dump(lcm_struct_t *lr)
{
    printf("struct %s [hash=0x%16"PRId64"]\n", lr->structname->typename, lr->hash);

    for (unsigned int i = 0; i < g_ptr_array_size(lr->members); i++) {
        lcm_member_t *lm = g_ptr_array_index(lr->members, i);
        lcm_member_dump(lm);
    }

    for (unsigned int i = 0; i < g_ptr_array_size(lr->enums); i++) {
        lcm_enum_t *le = g_ptr_array_index(lr->enums, i);
        lcm_enum_dump(le);
    }
}

void lcmgen_dump(lcmgen_t *lcmgen)
{
    for (unsigned int i = 0; i < g_ptr_array_size(lcmgen->enums); i++) {
        lcm_enum_t *le = g_ptr_array_index(lcmgen->enums, i);
        lcm_enum_dump(le);
    }

    for (unsigned int i = 0; i < g_ptr_array_size(lcmgen->structs); i++) {
        lcm_struct_t *lr = g_ptr_array_index(lcmgen->structs, i);
        lcm_struct_dump(lr);
    }
}

/** Find and return the member whose name is name. **/
lcm_member_t *lcm_find_member(lcm_struct_t *lr, const char *name)
{
    for (unsigned int i = 0; i < g_ptr_array_size(lr->members); i++) {
        lcm_member_t *lm = (lcm_member_t*) g_ptr_array_index(lr->members, i);
        if (!strcmp(lm->membername, name))
            return lm;
    }

    return NULL;
}

int lcm_needs_generation(lcmgen_t *lcmgen, const char *declaringfile, const char *outfile)
{
    struct stat instat, outstat;
    int res;

    if (!getopt_get_bool(lcmgen->gopt, "lazy"))
        return 1;

    res = stat(declaringfile, &instat);
    if (res) {
        printf("Funny error: can't stat the .lcm file");
        perror(declaringfile);
        return 1;
    }

    res = stat(outfile, &outstat);
    if (res)
        return 1;

    return instat.st_mtime > outstat.st_mtime;
}

/** Is the member an array of constant size? If it is not an array, it returns zero. **/
int lcm_is_constant_size_array(lcm_member_t *lm)
{
    int ndim = g_ptr_array_size(lm->dimensions);
    
    if (ndim == 0)
        return 1;

    for (int i = 0; i < ndim; i++) {
        lcm_dimension_t *dim = g_ptr_array_index(lm->dimensions, i);
        
        if (dim->mode == LCM_VAR)
            return 0;
    }

    return 1;
}