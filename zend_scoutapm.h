/*
 * Scout APM extension for PHP
 *
 * Copyright (C) 2019-
 * For license information, please see the LICENSE file.
 */

#ifndef ZEND_SCOUTAPM_H
#define ZEND_SCOUTAPM_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include <zend_extensions.h>
#include <zend_compile.h>
#include <zend_exceptions.h>
#include "ext/standard/php_var.h"

#define SCOUT_APM_EXT_NAME "scoutapm"
#define SCOUT_APM_EXT_VERSION "0.0.1"

/* Extreme amounts of debugging, set to 1 to enable it and `make clean && make` (tests will fail...) */
#define SCOUT_APM_EXT_DEBUGGING 0

PHP_FUNCTION(scoutapm_get_calls);

/* Describes information we store about a recorded stack frame */
typedef struct scoutapm_stack_frame {
    const char *function_name;
    double entered;
    double exited;
    int argc;
    zval *argv;
} scoutapm_stack_frame;

/* These are the "module globals". In non-ZTS mode, they're just regular variables, but means in ZTS mode they get handled properly */
ZEND_BEGIN_MODULE_GLOBALS(scoutapm)
    zend_bool handlers_set;
    zend_long observed_stack_frames_count;
    scoutapm_stack_frame *observed_stack_frames;
ZEND_END_MODULE_GLOBALS(scoutapm)

/* Accessor for "module globals" for non-ZTS and ZTS modes. */
#ifdef ZTS
#define SCOUTAPM_G(v) ZEND_MODULE_GLOBALS_ACCESSOR(scoutapm, v)
#else
#define SCOUTAPM_G(v) (scoutapm_globals.v)
#endif

/* zif_handler is not always defined, so define this roughly equivalent */
#ifndef zif_handler
typedef void (*zif_handler)(INTERNAL_FUNCTION_PARAMETERS);
#endif

/* Sometimes this isn't defined, so define it in that case */
#ifndef ZEND_PARSE_PARAMETERS_NONE
#define ZEND_PARSE_PARAMETERS_NONE() if (zend_parse_parameters_none() != SUCCESS) { return; }
#endif

/* The debugging toggle allows us to output unnecessary amounts of information. Because it's a preprocessor flag, this stuff is compiled out */
#if SCOUT_APM_EXT_DEBUGGING == 1
#define SCOUTAPM_DEBUG_MESSAGE(x, ...) php_printf(x, ##__VA_ARGS__)
#else
#define SCOUTAPM_DEBUG_MESSAGE(...) /**/
#endif

/* Shortcut defined to allow using a `char *` with snprintf - determine the size first, allocate, then snprintf */
#define DYNAMIC_MALLOC_SPRINTF(destString, sizeNeeded, fmt, ...) \
    sizeNeeded = snprintf(NULL, 0, fmt, ##__VA_ARGS__) + 1; \
    destString = (char*)malloc(sizeNeeded); \
    snprintf(destString, sizeNeeded, fmt, ##__VA_ARGS__)

/* overload a regular function by wrapping its handler with our own handler */
#define SCOUT_OVERLOAD_FUNCTION(function_name) \
    original_function = zend_hash_str_find_ptr(EG(function_table), function_name, sizeof(function_name) - 1); \
    if (original_function != NULL) { \
        handler_index = handler_index_for_function(function_name); \
        if (handler_index < 0) { \
            zend_throw_exception(NULL, "ScoutAPM did not define a handler index for "function_name, 0); \
            return FAILURE;\
        } \
        original_handlers[handler_index] = original_function->internal_function.handler; \
        original_function->internal_function.handler = scoutapm_overloaded_handler; \
    }

/* Don't use this macro directly, use SCOUT_OVERLOAD_STATIC_METHOD or SCOUT_OVERLOAD_METHOD for consistency */
#define SCOUT_OVERLOAD_CLASS_ENTRY_FUNCTION(lowercase_class_name, instance_or_static, method_name) \
    ce = zend_hash_str_find_ptr(CG(class_table), lowercase_class_name, sizeof(lowercase_class_name) - 1); \
    if (ce != NULL) { \
        original_function = zend_hash_str_find_ptr(&ce->function_table, method_name, sizeof(method_name)-1); \
        if (original_function != NULL) { \
            handler_index = handler_index_for_function(lowercase_class_name instance_or_static method_name); \
            if (handler_index < 0) { \
                zend_throw_exception(NULL, "ScoutAPM did not define a handler index for "lowercase_class_name instance_or_static method_name, 0); \
                return FAILURE; \
            } \
            original_handlers[handler_index] = original_function->internal_function.handler; \
            original_function->internal_function.handler = scoutapm_overloaded_handler; \
        } \
    }

/* overload a static class method by wrapping its handler with our own handler */
#define SCOUT_OVERLOAD_STATIC_METHOD(lowercase_class_name, method_name) SCOUT_OVERLOAD_CLASS_ENTRY_FUNCTION(lowercase_class_name, "::", method_name)

/* overload an instance class method by wrapping its handler with our own handler */
#define SCOUT_OVERLOAD_METHOD(lowercase_class_name, method_name) SCOUT_OVERLOAD_CLASS_ENTRY_FUNCTION(lowercase_class_name, "->", method_name)

/* these are the string keys used in scoutapm_get_calls associative array return value */
#define SCOUT_GET_CALLS_KEY_FUNCTION "function"
#define SCOUT_GET_CALLS_KEY_ENTERED "entered"
#define SCOUT_GET_CALLS_KEY_EXITED "exited"
#define SCOUT_GET_CALLS_KEY_TIME_TAKEN "time_taken"
#define SCOUT_GET_CALLS_KEY_ARGV "argv"

#endif /* ZEND_SCOUTAPM_H */
