#include <stdlib.h>
#include <string.h>

#include "comet.h"
#include "cometlib.h"

typedef struct StringData
{
    size_t length;
    char *chars;
} StringData;

void *string_constructor(void)
{
    StringData *data = ALLOCATE(StringData, 1);
    data->length = 0;
    data->chars = NULL;
    return (void *) data;
}

void string_destructor(void *data)
{
    StringData *string_data = (StringData *) data;
    if (string_data->chars != NULL)
    {
        FREE_ARRAY(char, string_data->chars, string_data->length + 1);
        string_data->chars = NULL;
        string_data->length = 0;
    }
    FREE(StringData, string_data);
}

const char *get_cstr(VALUE self)
{
    StringData *string_data = (StringData *) AS_NATIVE_INSTANCE(self)->data;
    return string_data->chars;
}

VALUE string_equals(VALUE UNUSED(self), int UNUSED(arg_count), VALUE UNUSED(*arguments))
{
    StringData *lhs = (StringData *) AS_NATIVE_INSTANCE(self)->data;
    StringData *rhs = (StringData *) AS_NATIVE_INSTANCE(arguments[0])->data;
    if (lhs->length != rhs->length)
        return FALSE_VAL;

    return BOOL_VAL(strncmp(lhs->chars, rhs->chars, lhs->length) == 0);
}

VALUE string_hash(VALUE UNUSED(self), int UNUSED(arg_count), VALUE UNUSED(*arguments))
{
    return NIL_VAL;
}

VALUE string_to_string(VALUE self, int UNUSED(arg_count), VALUE UNUSED(*arguments))
{
    return self;
}

VALUE string_trim(VALUE UNUSED(self), int UNUSED(arg_count), VALUE UNUSED(*arguments))
{
    return NIL_VAL;
}

VALUE string_trim_left(VALUE UNUSED(self), int UNUSED(arg_count), VALUE UNUSED(*arguments))
{
    return NIL_VAL;
}

VALUE string_trim_right(VALUE UNUSED(self), int UNUSED(arg_count), VALUE UNUSED(*arguments))
{
    return NIL_VAL;
}

VALUE string_find(VALUE UNUSED(self), int UNUSED(arg_count), VALUE UNUSED(*arguments))
{
    return NIL_VAL;
}

VALUE string_split(VALUE UNUSED(self), int UNUSED(arg_count), VALUE UNUSED(*arguments))
{
    return NIL_VAL;
}

VALUE string_replace(VALUE UNUSED(self), int UNUSED(arg_count), VALUE UNUSED(*arguments))
{
    return NIL_VAL;
}

VALUE string_starts_with_q(VALUE UNUSED(self), int UNUSED(arg_count), VALUE UNUSED(*arguments))
{
    return NIL_VAL;
}

VALUE string_ends_with_q(VALUE UNUSED(self), int UNUSED(arg_count), VALUE UNUSED(*arguments))
{
    return NIL_VAL;
}

VALUE string_to_lower(VALUE UNUSED(self), int UNUSED(arg_count), VALUE UNUSED(*arguments))
{
    return NIL_VAL;
}

VALUE string_to_upper(VALUE UNUSED(self), int UNUSED(arg_count), VALUE UNUSED(*arguments))
{
    return NIL_VAL;
}

VALUE string_empty_q(VALUE self, int UNUSED(arg_count), VALUE UNUSED(*arguments))
{
    StringData *data = (StringData *) AS_NATIVE_INSTANCE(self)->data;
    return BOOL_VAL(data->length == 0);
}

VALUE string_concatenate(VALUE UNUSED(self), int UNUSED(arg_count), VALUE UNUSED(*arguments))
{
    return NIL_VAL;
}

void init_string(void)
{
    VALUE klass = defineNativeClass("String", string_constructor, string_destructor, NULL);
    defineNativeMethod(klass, &string_hash, "hash", false);
    defineNativeMethod(klass, &string_to_string, "to_string", false);
    defineNativeMethod(klass, &string_trim, "trim", false);
    defineNativeMethod(klass, &string_trim_left, "left_trim", false);
    defineNativeMethod(klass, &string_trim_right, "right_trim", false);
    defineNativeMethod(klass, &string_find, "find", false);
    defineNativeMethod(klass, &string_split, "split", false);
    defineNativeMethod(klass, &string_replace, "replace", false);
    defineNativeMethod(klass, &string_starts_with_q, "starts_with?", false);
    defineNativeMethod(klass, &string_ends_with_q, "ends_with?", false);
    defineNativeMethod(klass, &string_empty_q, "empty?", false);
    defineNativeMethod(klass, &string_to_lower, "to_lower", false);
    defineNativeMethod(klass, &string_to_upper, "to_upper", false);
    defineNativeOperator(klass, &string_concatenate, OPERATOR_PLUS);
    defineNativeOperator(klass, &string_equals, OPERATOR_EQUALS);

    registerStringClass(klass);
}