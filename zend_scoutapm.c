/*
 * Scout APM extension for PHP
 *
 * Copyright (C) 2019-
 * For license information, please see the LICENSE file.
 */

#include "zend_scoutapm.h"

static PHP_RINIT_FUNCTION(scoutapm);
static PHP_RSHUTDOWN_FUNCTION(scoutapm);
static int zend_scoutapm_startup(zend_extension*);
static double scoutapm_microtime();
static void record_observed_stack_frame(const char *function_name, double microtime_entered, double microtime_exited, int argc, zval *argv);
static int handler_index_for_function(const char *function_to_lookup);
static const char* determine_function_name(zend_execute_data *execute_data);

/* This is simply a map of function names to an index in original_handlers */
struct {
    const char *function_name;
    int index;
} handler_lookup[] = {
    /* define each function we want to overload, which maps to an index in the `original_handlers` array */
    {"file_get_contents", 0},
    {"file_put_contents", 1},
    {"curl_exec", 2},
    {"fread", 3},
    {"fwrite", 4},
    {"pdo->exec", 5},
    {"pdo->query", 6},
    {"pdostatement->execute", 7},
};
/* handlers count needs to be the number of handler lookups defined above. */
zif_handler original_handlers[8];

ZEND_DECLARE_MODULE_GLOBALS(scoutapm)

/* a PHP module defines what functions it exports */
static const zend_function_entry scoutapm_functions[] = {
    PHP_FE(scoutapm_get_calls, NULL)
    PHP_FE_END
};

/* scoutapm_module_entry provides the metadata/information for PHP about this PHP module */
static zend_module_entry scoutapm_module_entry = {
    STANDARD_MODULE_HEADER,
    SCOUT_APM_EXT_NAME,
    scoutapm_functions,             /* function entries */
    NULL,                           /* module init */
    NULL,                           /* module shutdown */
    PHP_RINIT(scoutapm),            /* request init */
    PHP_RSHUTDOWN(scoutapm),        /* request shutdown */
    NULL,                           /* module information */
    SCOUT_APM_EXT_VERSION,          /* module version */
    PHP_MODULE_GLOBALS(scoutapm),   /* module global variables */
    NULL,
    NULL,
    NULL,
    STANDARD_MODULE_PROPERTIES_EX
};

/*
 * Do not export this module, so it cannot be registered with `extension=scoutapm.so` - must be `zend_extension=`
 * Instead, see `zend_scoutapm_startup` - we load the module there.
ZEND_GET_MODULE(scoutapm);
 */

/* extension_version_info is used by PHP */
zend_extension_version_info extension_version_info = {
    ZEND_EXTENSION_API_NO,
    (char*) ZEND_EXTENSION_BUILD_ID
};

/* zend_extension_entry provides the metadata/information for PHP about this zend extension */
zend_extension zend_extension_entry = {
    (char*) SCOUT_APM_EXT_NAME,
    (char*) SCOUT_APM_EXT_VERSION,
    (char*) "Scout APM",
    (char*) "https://scoutapm.com/",
    (char*) "Copyright 2019",
    zend_scoutapm_startup,  /* extension startup */
    NULL,                   /* extension shutdown */
    NULL,                   /* request startup */
    NULL,                   /* request shutdown */
    NULL,                   /* message handler */
    NULL,                   /* compiler op_array_ahndler */
    NULL,                   /* VM statement_handler */
    NULL,                   /* VM fcall_begin_handler */
    NULL,                   /* VM_fcall_end_handler */
    NULL,                   /* compiler op_array_ctor */
    NULL,                   /* compiler op_array_dtor */
    STANDARD_ZEND_EXTENSION_PROPERTIES
};

/*
 * This is a hybrid "Zend Extension / PHP Module", and because the PHP module is not exported, we become responsible
 * for starting up the module. For more details on the PHP/Zend extension lifecycle, see:
 * http://www.phpinternalsbook.com/php7/extensions_design/zend_extensions.html#hybrid-extensions
 */
static int zend_scoutapm_startup(zend_extension *ze) {
    return zend_startup_module(&scoutapm_module_entry);
}

/*
 * This handles ALL recorded calls by grabbing all arguments (we treat it as a variadic), finding the "original" handler
 * for the function and calling it. Once the function is called, record how long it took.
 */
ZEND_NAMED_FUNCTION(scoutapm_overloaded_handler)
{
    int handler_index;
    double entered = scoutapm_microtime();
    int argc;
    zval *argv = NULL;
    const char *called_function;

    /* note - no strdup needed as we copy it in record_observed_stack_frame */
    called_function = determine_function_name(execute_data);

    ZEND_PARSE_PARAMETERS_START(0, -1)
        Z_PARAM_VARIADIC(' ', argv, argc)
    ZEND_PARSE_PARAMETERS_END();

    handler_index = handler_index_for_function(called_function);

    /* Practically speaking, this shouldn't happen as long as we defined the handlers properly */
    if (handler_index < 0) {
        zend_throw_exception(NULL, "ScoutAPM overwrote a handler for a function it didn't define a handler for", 0);
        return;
    }

    original_handlers[handler_index](INTERNAL_FUNCTION_PARAM_PASSTHRU);

    record_observed_stack_frame(called_function, entered, scoutapm_microtime(), argc, argv);
}

/*
 * Set up the handlers and stack. This is called at the start of each request to PHP.
 */
static PHP_RINIT_FUNCTION(scoutapm)
{
    zend_function *original_function;
    int handler_index;
    zend_class_entry *ce;

    SCOUTAPM_DEBUG_MESSAGE("Initialising stacks...");
    SCOUTAPM_G(observed_stack_frames_count) = 0;
    SCOUTAPM_G(observed_stack_frames) = calloc(0, sizeof(scoutapm_stack_frame));
    SCOUTAPM_DEBUG_MESSAGE("Stacks made\n");

    if (SCOUTAPM_G(handlers_set) != 1) {
        SCOUTAPM_DEBUG_MESSAGE("Overriding function handlers.\n");

        /* @todo make overloaded functions configurable? https://github.com/scoutapp/scout-apm-php-ext/issues/30 */
        SCOUT_OVERLOAD_FUNCTION("file_get_contents")
        SCOUT_OVERLOAD_FUNCTION("file_put_contents")
        SCOUT_OVERLOAD_FUNCTION("curl_exec")
        SCOUT_OVERLOAD_FUNCTION("fwrite")
        SCOUT_OVERLOAD_FUNCTION("fread")
        SCOUT_OVERLOAD_METHOD("pdo", "exec")
        SCOUT_OVERLOAD_METHOD("pdo", "query")
        SCOUT_OVERLOAD_METHOD("pdostatement", "execute")

        SCOUTAPM_G(handlers_set) = 1;
    } else {
        SCOUTAPM_DEBUG_MESSAGE("Handlers have already been set, skipping.\n");
    }

    return SUCCESS;
}

/*
 * At the end of the request, this function is responsible for freeing all memory we allocated. Note that if we forget
 * to free something, if PHP is compiled with --enable-debug, then we will see memory leaks.
 */
static PHP_RSHUTDOWN_FUNCTION(scoutapm)
{
    SCOUTAPM_DEBUG_MESSAGE("Freeing stacks... ");

    for (int i = 0; i < SCOUTAPM_G(observed_stack_frames_count); i++) {
        for (int j = 0; j < SCOUTAPM_G(observed_stack_frames)[i].argc; j++) {
            zval_ptr_dtor(&(SCOUTAPM_G(observed_stack_frames)[i].argv[j]));
        }
        free(SCOUTAPM_G(observed_stack_frames)[i].argv);
        free((void*)SCOUTAPM_G(observed_stack_frames)[i].function_name);
    }

    if (SCOUTAPM_G(observed_stack_frames)) {
        free(SCOUTAPM_G(observed_stack_frames));
    }
    SCOUTAPM_G(observed_stack_frames_count) = 0;
    SCOUTAPM_DEBUG_MESSAGE("Stacks freed\n");

    return SUCCESS;
}

/*
 * Given some zend_execute_data, figure out what the function/method/static method is being called. The convention used
 * is `ClassName::methodName` for static methods, `ClassName->methodName` for instance methods, and `functionName` for
 * regular functions.
 */
static const char* determine_function_name(zend_execute_data *execute_data)
{
    int len;
    char *ret;

    if (!execute_data->func) {
        return "<not a function call>";
    }

    if (execute_data->func->common.fn_flags & ZEND_ACC_STATIC) {
        DYNAMIC_MALLOC_SPRINTF(ret, len, "%s::%s",
            ZSTR_VAL(Z_CE(execute_data->This)->name),
            ZSTR_VAL(execute_data->func->common.function_name)
        );
        return ret;
    }

    if (Z_TYPE(execute_data->This) == IS_OBJECT) {
        DYNAMIC_MALLOC_SPRINTF(ret, len, "%s->%s",
            ZSTR_VAL(Z_OBJCE(execute_data->This)->name),
            ZSTR_VAL(execute_data->func->common.function_name)
        );
        return ret;
    }

    return ZSTR_VAL(execute_data->func->common.function_name);
}

/*
 * In our handler_lookup, find what the "index" in our overridden handlers is for a particular function name
 */
static int handler_index_for_function(const char *function_to_lookup)
{
    int i = 0;
    const char *current = handler_lookup[i].function_name;
    /* @todo fix https://github.com/scoutapp/scout-apm-php-ext/issues/29 */
    while (current) {
        if (strcasecmp(current, function_to_lookup) == 0) {
            return handler_lookup[i].index;
        }
        current = handler_lookup[++i].function_name;
    }

    return -1;
}

/*
 * Using gettimeofday, determine the time using microsecond precision, and return as a double.
 * @todo consider using HR monotonic time? https://github.com/scoutapp/scout-apm-php-ext/issues/23
 */
static double scoutapm_microtime()
{
    struct timeval tp = {0};
    if (gettimeofday(&tp, NULL)) {
        zend_throw_exception_ex(zend_ce_exception, 0, "Could not call gettimeofday");
        return 0;
    }
    return (double)(tp.tv_sec + tp.tv_usec / 1000000.00);
}

/*
 * Helper function to handle memory allocation for recorded stack frames. Called each time a function has completed
 * that we're interested in.
 */
static void record_observed_stack_frame(const char *function_name, double microtime_entered, double microtime_exited, int argc, zval *argv)
{
    if (argc > 0) {
        SCOUTAPM_DEBUG_MESSAGE("Adding observed stack frame for %s (%s) ... ", function_name, Z_STRVAL(argv[0]));
    } else {
        SCOUTAPM_DEBUG_MESSAGE("Adding observed stack frame for %s ... ", function_name);
    }
    SCOUTAPM_G(observed_stack_frames) = realloc(
        SCOUTAPM_G(observed_stack_frames),
        (SCOUTAPM_G(observed_stack_frames_count)+1) * sizeof(scoutapm_stack_frame)
    );

    SCOUTAPM_G(observed_stack_frames)[SCOUTAPM_G(observed_stack_frames_count)].function_name = strdup(function_name);
    SCOUTAPM_G(observed_stack_frames)[SCOUTAPM_G(observed_stack_frames_count)].entered = microtime_entered;
    SCOUTAPM_G(observed_stack_frames)[SCOUTAPM_G(observed_stack_frames_count)].exited = microtime_exited;
    SCOUTAPM_G(observed_stack_frames)[SCOUTAPM_G(observed_stack_frames_count)].argc = argc;
    SCOUTAPM_G(observed_stack_frames)[SCOUTAPM_G(observed_stack_frames_count)].argv = calloc(argc, sizeof(zval));

    for (int i = 0; i < argc; i++) {
        ZVAL_COPY(
            &(SCOUTAPM_G(observed_stack_frames)[SCOUTAPM_G(observed_stack_frames_count)].argv[i]),
            &(argv[i])
        );
    }

    SCOUTAPM_G(observed_stack_frames_count)++;
    SCOUTAPM_DEBUG_MESSAGE("Done\n");
}

/* {{{ proto array scoutapm_get_calls()
   Fetch all the recorded function or method calls recorded by the ScoutAPM extension. */
PHP_FUNCTION(scoutapm_get_calls)
{
    zval item, arg_items, arg_item;
    ZEND_PARSE_PARAMETERS_NONE();

    SCOUTAPM_DEBUG_MESSAGE("scoutapm_get_calls: preparing return value... ");

    array_init(return_value);

    for (int i = 0; i < SCOUTAPM_G(observed_stack_frames_count); i++) {
        array_init(&item);

        add_assoc_str_ex(
            &item,
            SCOUT_GET_CALLS_KEY_FUNCTION, strlen(SCOUT_GET_CALLS_KEY_FUNCTION),
            zend_string_init(SCOUTAPM_G(observed_stack_frames)[i].function_name, strlen(SCOUTAPM_G(observed_stack_frames)[i].function_name), 0)
        );

        add_assoc_double_ex(
            &item,
            SCOUT_GET_CALLS_KEY_ENTERED, strlen(SCOUT_GET_CALLS_KEY_ENTERED),
            SCOUTAPM_G(observed_stack_frames)[i].entered
        );

        add_assoc_double_ex(
            &item,
            SCOUT_GET_CALLS_KEY_EXITED, strlen(SCOUT_GET_CALLS_KEY_EXITED),
            SCOUTAPM_G(observed_stack_frames)[i].exited
        );

        /* Time taken is calculated here because float precision gets knocked off otherwise - so this is a useful metric */
        add_assoc_double_ex(
            &item,
            SCOUT_GET_CALLS_KEY_TIME_TAKEN, strlen(SCOUT_GET_CALLS_KEY_TIME_TAKEN),
            SCOUTAPM_G(observed_stack_frames)[i].exited - SCOUTAPM_G(observed_stack_frames)[i].entered
        );

        array_init(&arg_items);
        for (int j = 0; j < SCOUTAPM_G(observed_stack_frames)[i].argc; j++) {
            /* Must copy the argument to a new zval, otherwise it gets freed and we get segfault. */
            ZVAL_COPY(&arg_item, &(SCOUTAPM_G(observed_stack_frames)[i].argv[j]));
            add_next_index_zval(&arg_items, &arg_item);
            zval_ptr_dtor(&(SCOUTAPM_G(observed_stack_frames)[i].argv[j]));
        }
        free(SCOUTAPM_G(observed_stack_frames)[i].argv);

        add_assoc_zval_ex(
            &item,
            SCOUT_GET_CALLS_KEY_ARGV, strlen(SCOUT_GET_CALLS_KEY_ARGV),
            &arg_items
        );

        add_next_index_zval(return_value, &item);

        free((void*)SCOUTAPM_G(observed_stack_frames)[i].function_name);
    }

    SCOUTAPM_G(observed_stack_frames) = realloc(SCOUTAPM_G(observed_stack_frames), 0);
    SCOUTAPM_G(observed_stack_frames_count) = 0;

    SCOUTAPM_DEBUG_MESSAGE("done.\n");
}
/* }}} */
