/*****************************************************************************
 * Licensed to Qualys, Inc. (QUALYS) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * QUALYS licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *****************************************************************************/

#include <ironbee/core.h>
#include <ironbee/cfgmap.h>
#include <ironbee/debug.h>
#include <ironbee/engine.h>
#include <ironbee/lock.h>
#include <ironbee/module.h>
#include <ironbee/provider.h>
#include <ironbee/util.h>
#include <ironbee/list.h>
#include <ironbee/config.h>
#include <ironbee/mpool.h>
#include <ironbee/rule_engine.h>
#include <ironbee/operator.h>
#include <ironbee/action.h>

#include <rules_lua.h>

#include "lua/ironbee.h"
//#if defined(__cplusplus) && !defined(__STDC_FORMAT_MACROS)
/* C99 requires that inttypes.h only exposes PRI* macros
 *  * for C++ implementations if this is defined: */
//#define __STDC_FORMAT_MACROS
//#endif
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/stat.h>
#include <ctype.h>

#include <lua.h>

/* Define the module name as well as a string version of it. */
#define MODULE_NAME        rules
#define MODULE_NAME_STR    IB_XSTRINGIFY(MODULE_NAME)

/* Declare the public module symbol. */
IB_MODULE_DECLARE();

/**
 * Ironbee's root rule state.
 */
static lua_State *g_ironbee_rules_lua;

/**
 * @brief Semaphore ID used to protect Lua thread creation and destruction.
 */
static ib_lock_t g_lua_lock;


/**
 * @brief Callback type for functions executed protected by g_lua_lock.
 * @details This callback should take a @c ib_engine_t* which is used
 *          for logging, @c a lua_State* which is used to create the
 *          new thread, and a @c lua_State** which will be assigned a
 *          new @c lua_State*.
 */
typedef ib_status_t(*critical_section_fn_t)(ib_engine_t*,
                                            lua_State*,
                                            lua_State**);


/**
 * @internal
 * Parse rule's operator.
 *
 * Parses the rule's operator string @a str and, stores the results in the
 * rule object @a rule.
 *
 * @param cp IronBee configuration parser
 * @param rule Rule object to update
 * @param str Operator string
 *
 * @returns Status code
 */
static ib_status_t parse_operator(ib_cfgparser_t *cp,
                                  ib_rule_t *rule,
                                  const char *str)
{
    IB_FTRACE_INIT();
    ib_status_t rc = IB_OK;
    const char *at = NULL;
    ib_num_t bang = 0;
    const char *op = NULL;
    const char *cptr;
    ib_flags_t flags = IB_OPINST_FLAG_NONE;
    char *copy;
    char *space;
    char *args = NULL;
    ib_operator_inst_t *operator;

    /* Search for leading '!' / '@' */
    for (cptr = str;  *cptr != '\0';  cptr++) {
        if ( (at == NULL) && (bang == 0) && (*cptr == '!') ) {
            bang = 1;
            flags |= IB_OPINST_FLAG_INVERT;
        }
        else if ( (at == NULL) && (*cptr == '@') ) {
            at = cptr;
            break;
        }
        else if (isblank(*cptr) == 0) {
            ib_log_error(cp->ib, 4, "Invalid rule syntax '%s'", str);
            IB_FTRACE_RET_STATUS(IB_EINVAL);
        }
    }

    /* Make sure that we have an operator */
    if (! at || strlen(at+1) == 0) {
        ib_log_error(cp->ib, 4, "Invalid rule syntax '%s'", str);
        IB_FTRACE_RET_STATUS(IB_EINVAL);
    }

    /* Make a copy of the string to operate on */
    copy = ib_mpool_strdup(ib_rule_mpool(cp->ib), at+1);
    if (copy == NULL) {
        ib_log_error(cp->ib, 4, "Failed to copy rule operator string '%s'", at);
        IB_FTRACE_RET_STATUS(IB_EALLOC);
    }
    op = copy;

    /* Find first space */
    space = strchr(copy, ' ');
    if (space != NULL) {
        size_t  alen;

        /* Find the first non-whitespace */
        args = space;
        while( isspace(*args) ) {
            ++args;
        }

        /* Mark the end of the operator itself with a NUL */
        *space = '\0';

        /* Strip off trailing whitespace from args */
        alen = strlen(args);
        if (alen > 0) {
            char *end = args+alen-1;
            while( (end > args) && ( *end == ' ') ) {
                *end = '\0';
                --end;
            }
        }

        /* Is args an empty string? */
        if (*args == '\0') {
            args = NULL;
        }
    }

    /* Create the operator instance */
    rc = ib_operator_inst_create(
        cp->ib, cp->cur_ctx, op, args, flags, &operator);
    if (rc != IB_OK) {
        ib_log_error(cp->ib, 4,
                     "Failed to create operator instance '%s': %d", op, rc);
        IB_FTRACE_RET_STATUS(rc);
    }

    /* Set the operator */
    rc = ib_rule_set_operator(cp->ib, rule, operator);
    if (rc != IB_OK) {
        ib_log_error(cp->ib, 4,
                     "Failed to set operator for rule: %d", rc);
        IB_FTRACE_RET_STATUS(rc);
    }
    ib_log_debug(cp->ib, 9,
                 "Rule: op='%s'; flags=0x%04x args='%s'",
                 op, flags, ( (args == NULL) ? "" : args) );

    IB_FTRACE_RET_STATUS(rc);
}


/**
 * @internal
 * Parse a rule's input string.
 *
 * Parses the rule's input field list string @a input_str, and stores the
 * results in the rule object @a rule.
 *
 * @param cp IronBee configuration parser
 * @param rule Rule to operate on
 * @param input_str Input field name.
 *
 * @returns Status code
 */
static ib_status_t parse_inputs(ib_cfgparser_t *cp,
                                ib_rule_t *rule,
                                const char *input_str)
{
    IB_FTRACE_INIT();
    ib_status_t rc = IB_OK;
    const char *cur;
    char *copy;

    /* Copy the input string */
    while(isspace(*input_str)) {
        ++input_str;
    }
    if (*input_str == '\0') {
        ib_log_error(cp->ib, 4, "Rule inputs is empty");
        IB_FTRACE_RET_STATUS(IB_EINVAL);
    }
    copy = ib_mpool_strdup(ib_rule_mpool(cp->ib), input_str);
    if (copy == NULL) {
        ib_log_error(cp->ib, 4, "Failed to copy rule inputs");
        IB_FTRACE_RET_STATUS(IB_EALLOC);
    }

    /* Split it up */
    for (cur = strtok(copy, "|,");
         cur != NULL;
         cur = strtok(NULL, "|,") ) {
        rc = ib_rule_add_input(cp->ib, rule, cur);
        if (rc != IB_OK) {
            ib_log_error(cp->ib, 4, "Failed to add rule input '%s'", cur);
            IB_FTRACE_RET_STATUS(rc);
        }
        ib_log_debug(cp->ib, 4,
                     "Added rule input '%s' to rule %p", cur, (void*)rule);
    }

    IB_FTRACE_RET_STATUS(rc);
}

/**
 * @internal
 * Parse a rule's modifier string.
 *
 * Parses the rule's modifier string @a modifier_str, and stores the results
 * in the rule object @a rule.
 *
 * @param[in] cp IronBee configuration parser
 * @param[in,out] rule Rule to operate on
 * @param[out] phase Rule phase in which the rule should be executed.
 * @param[in] modifier_str Input field name.
 *
 * @returns Status code
 */
static ib_status_t parse_modifier(ib_cfgparser_t *cp,
                                  ib_rule_t *rule,
                                  ib_rule_phase_t *phase,
                                  const char *modifier_str)
{
    IB_FTRACE_INIT();
    ib_status_t rc = IB_OK;
    const char *name;
    char *colon;
    char *copy;
    const char *value = NULL;

    /* Copy the string */
    copy = ib_mpool_strdup(ib_rule_mpool(cp->ib), modifier_str);
    if (copy == NULL) {
        ib_log_error(cp->ib, 4,
                     "Failed to copy rule modifier '%s'", modifier_str);
        IB_FTRACE_RET_STATUS(IB_EALLOC);
    }

    /* Modifier name */
    name = copy;
    colon = strchr(copy, ':');
    if ( (colon != NULL) && ( *(colon+1) != '\0' ) ) {
        *colon = '\0';
        value = colon + 1;
        while( isspace(*value) ) {
            value++;
        }
        if (*value == '\0') {
            value = NULL;
        }
    }

    /* ID modifier */
    if (strcasecmp(name, "id") == 0) {
        if (value == NULL) {
            ib_log_error(cp->ib, 4, "Modifier ID with no value");
            IB_FTRACE_RET_STATUS(IB_EINVAL);
        }
        ib_rule_set_id(cp->ib, rule, value);
    }
    else if (strcasecmp(name, "phase") == 0) {
        if (value == NULL) {
            ib_log_error(cp->ib, 4, "Modifier PHASE with no value");
            IB_FTRACE_RET_STATUS(IB_EINVAL);
        }
        else if (strcasecmp(value,"REQUEST_HEADER") == 0) {
            *phase = PHASE_REQUEST_HEADER;
        }
        else if (strcasecmp(value,"REQUEST") == 0) {
            *phase = PHASE_REQUEST_BODY;
        }
        else if (strcasecmp(value,"RESPONSE_HEADER") == 0) {
            *phase = PHASE_RESPONSE_HEADER;
        }
        else if (strcasecmp(value,"RESPONSE") == 0) {
            *phase = PHASE_RESPONSE_BODY;
        }
        else if (strcasecmp(value,"POSTPROCESS") == 0) {
            *phase = PHASE_POSTPROCESS;
        }
        else if (strcasecmp(value,"NONE") == 0) {
            *phase = PHASE_NONE;
        }
        else {
            ib_log_error(cp->ib, 4, "Invalid PHASE modifier '%s'", value);
            IB_FTRACE_RET_STATUS(IB_EINVAL);
        }
    }
    else if (strcasecmp(name, "chain") == 0) {
        ib_rule_update_flags(cp->ib, rule, FLAG_OP_OR, IB_RULE_FLAG_CHAIN);
    }
    else {
        ib_action_inst_t  *action;
        ib_rule_action_t   atype = RULE_ACTION_TRUE;
        if (*name == '!') {
            name++;
            atype = RULE_ACTION_FALSE;
        }

        /* Create a new action instance */
        rc = ib_action_inst_create(
            cp->ib, cp->cur_ctx, name, value, IB_ACTINST_FLAG_NONE, &action);
        if (rc == IB_EINVAL) {
            ib_log_error(cp->ib, 4, "Unknown modifier %s", name);
            IB_FTRACE_RET_STATUS(IB_EINVAL);
        }
        else if (rc != IB_OK) {
            ib_log_error(cp->ib, 4,
                         "Failed to create action instance '%s': %d", name, rc);
            IB_FTRACE_RET_STATUS(rc);
        }

        /* Add the action to the rule */
        rc = ib_rule_add_action(cp->ib, rule, action, atype);
        if (rc != IB_OK) {
            ib_log_error(cp->ib, 4,
                         "Failed to add action %s to rule '%s': %d",
                         name, rc);
            IB_FTRACE_RET_STATUS(rc);
        }
    }

    IB_FTRACE_RET_STATUS(rc);
}

/**
 * @brief This will use @c g_lua_lock to atomically call @a fn.
 * @details The argument @a fn will be either
 *          ib_lua_new_thread(ib_engine_t*, lua_State**) or
 *          ib_lua_join_thread(ib_engine_t*, lua_State**) which will be called
 *          only if @c g_lua_lock can be locked using @c semop.
 * @param[in] ib IronBee context. Used for logging.
 * @param[in] fn The function to execute. This is passed @a ib and @a fn.
 * @param[in,out] L The Lua State to create or destroy. Passed to @a fn.
 * @returns If any error locking or unlocking the
 *          semaphore is encountered, the error code is returned.
 *          Otherwise the result of @a fn is returned.
 */
static ib_status_t call_in_critical_section(ib_engine_t *ib,
                                            critical_section_fn_t fn,
                                            lua_State **L)
{
    IB_FTRACE_INIT();

    /* Return code from IronBee calls. */
    ib_status_t ib_rc;
    /* Return code form critical call. */
    ib_status_t critical_rc;

    ib_rc  = ib_lock_lock(&g_lua_lock);

    /* Report semop error and return. */
    if (ib_rc != IB_OK) {
        ib_log_error(ib, 1, "Failed to lock Lua context.");
        IB_FTRACE_RET_STATUS(ib_rc);
    }

    /* Execute lua call in critical section. */
    critical_rc = fn(ib, g_ironbee_rules_lua, L);

    ib_rc = ib_lock_unlock(&g_lua_lock);

    if (critical_rc != IB_OK) {
        ib_log_error(ib, 1, "Critical call failed: %d", critical_rc);
    }

    /* Report semop error and return. */
    if (ib_rc != IB_OK) {
        ib_log_error(ib, 1, "Failed to unlock Lua context.");
        IB_FTRACE_RET_STATUS(ib_rc);
    }

    IB_FTRACE_RET_STATUS(critical_rc);
}

/**
 * @brief Call the rule named @a func_name on a new Lua stack.
 * @details This will atomically create and destroy a lua_State*
 *          allowing for concurrent execution of @a func_name
 *          by a ib_lua_func_eval(ib_engine_t*, ib_txt_t*, const char*).
 * @param[in] ib IronBee context.
 * @param[in,out] tx The transaction. The Rule may color this with data.
 * @param[in] func_name The Lua function name to call.
 * @param[out] result The result integer value. This should be set to
 *             1 (true) or 0 (false).
 * @returns IB_OK on success, IB_EUNKNOWN on semaphore locking error, and
 *          IB_EALLOC is returned if a new execution stack cannot be created.
 */
static ib_status_t ib_lua_func_eval_r(ib_engine_t *ib,
                                      ib_tx_t *tx,
                                      const char *func_name,
                                      ib_num_t *result)
{
    IB_FTRACE_INIT();

    int result_int;
    ib_status_t ib_rc;
    lua_State *L;

    /* Atomically create a new Lua stack */
    ib_rc = call_in_critical_section(ib, &ib_lua_new_thread, &L);

    if (ib_rc != IB_OK) {
        IB_FTRACE_RET_STATUS(ib_rc);
    }

    ib_log_debug(ib, 1, "Calling lua function in new thread %s", func_name);

    /* Call the rule in isolation. */
    ib_rc = ib_lua_func_eval_int(ib, tx, L, func_name, &result_int);

    /* Convert the passed in integer type to an ib_num_t. */
    *result = result_int;

    if (ib_rc != IB_OK) {
        IB_FTRACE_RET_STATUS(ib_rc);
    }

    /* Atomically destroy the Lua stack */
    ib_rc = call_in_critical_section(ib, &ib_lua_join_thread, &L);

    IB_FTRACE_RET_STATUS(ib_rc);
}

static ib_status_t lua_operator_create(ib_engine_t *ib,
                                       ib_context_t *ctx,
                                       ib_mpool_t *pool,
                                       const char *parameters,
                                       ib_operator_inst_t *op_inst)
{
    IB_FTRACE_INIT();
    IB_FTRACE_RET_STATUS(IB_OK);
}

static ib_status_t lua_operator_execute(ib_engine_t *ib,
                                        ib_tx_t *tx,
                                        void *data,
                                        ib_field_t *field,
                                        ib_num_t *result)
{
    IB_FTRACE_INIT();
    ib_status_t ib_rc;
    const char *func_name = (char*) data;

    ib_log_debug(ib, 1, "Calling lua function %s.", func_name);

    ib_rc = ib_lua_func_eval_r(ib, tx, func_name, result);

    ib_log_debug(ib, 1, "Calling to lua function %s=%d.", func_name, *result);

    IB_FTRACE_RET_STATUS(ib_rc);
}

static ib_status_t lua_operator_destroy(ib_operator_inst_t *op_inst)
{
    IB_FTRACE_INIT();
    IB_FTRACE_RET_STATUS(IB_OK);
}


/**
 * @brief Parse a RuleExt directive.
 * @details Register lua function. RuleExt lua:/path/to/rule.lua phase:REQUEST
 * @param[in,out] cp Configuration parser that contains the engine being
 *                configured.
 * @param[in] name The directive name.
 * @param[in] vars The list of variables passed to @c name.
 * @param[in] cbdata User data. Unused.
 */
static ib_status_t rules_ruleext_params(ib_cfgparser_t *cp,
                                        const char *name,
                                        const ib_list_t *vars,
                                        void *cbdata)
{
    IB_FTRACE_INIT();

    ib_status_t rc;
    const ib_list_node_t *inputs;
    const ib_list_node_t *mod;
    ib_rule_t *rule;
    ib_rule_phase_t phase = PHASE_NONE;
    ib_operator_inst_t *op_inst;
    const char *file_name;

    /* Get the inputs string */
    inputs = ib_list_first_const(vars);

    file_name = (const char*)ib_list_node_data_const(inputs);

    if ( file_name == NULL ) {
        ib_log_error(cp->ib, 1, "No inputs for rule");
        IB_FTRACE_RET_STATUS(IB_EINVAL);
    }

    ib_log_debug(cp->ib, 1, "Processing ext rule string %s", file_name);

    /* Allocate a rule */
    rc = ib_rule_create(cp->ib, cp->cur_ctx, &rule);
    if (rc != IB_OK) {
        ib_log_error(cp->ib, 1, "Failed to allocate rule: %d", rc);
        IB_FTRACE_RET_STATUS(rc);
    }
    ib_rule_update_flags(cp->ib, rule, FLAG_OP_OR, IB_RULE_FLAG_EXTERNAL);

    /* Parse all of the modifiers */
    mod = inputs;
    while( (mod = ib_list_node_next_const(mod)) != NULL) {
        ib_log_debug(cp->ib, 1, "Parsing modifier %s", mod->data);
        rc = parse_modifier(cp, rule, &phase, mod->data);
        if (rc != IB_OK) {
            ib_log_error(cp->ib, 1,
               "Error parsing rule modifier - \"%s\".",
                mod->data);
            IB_FTRACE_RET_STATUS(rc);
        }
    }

    /* Using the rule->meta and file_name, load and stage the ext rule. */
    if (!strncasecmp(file_name, "lua:", 4)) {
        rc = ib_lua_load_func(cp->ib,
                             g_ironbee_rules_lua,
                             file_name+4,
                             ib_rule_id(rule));

        if (rc != IB_OK) {
            ib_log_error(cp->ib, 1,
                "Failed to load ironbee file %s", file_name+4);
            IB_FTRACE_RET_STATUS(rc);
        }

        ib_log_debug(cp->ib, 1, "Loaded IronBee file %s", file_name+4);

        rc = ib_operator_register(cp->ib, file_name, 0,
                                 &lua_operator_create,
                                 &lua_operator_destroy,
                                 &lua_operator_execute);
        if (rc != IB_OK) {
            ib_log_error(cp->ib, 1,
                "Failed to register ironbee lua operator %s", file_name);
            IB_FTRACE_RET_STATUS(rc);
        }

        ib_log_debug(cp->ib, 1, "Registered IronBee operator %s", file_name);

        rc = ib_operator_inst_create(
            cp->ib, cp->cur_ctx, file_name, NULL, 0, &op_inst);

        if (rc != IB_OK) {
            ib_log_error(cp->ib, 1,
                "Failed to instantiate operator for rule %s", file_name+4);
            IB_FTRACE_RET_STATUS(rc);
        }

        ib_log_debug(cp->ib, 1, "Instantiated operator %s", file_name);

        /* The data is then name of the function. */
        op_inst->data = (void*)ib_rule_id(rule);

        rc = ib_rule_set_operator(cp->ib, rule, op_inst);

        if (rc != IB_OK) {
            ib_log_error(cp->ib, 1,
                "Failed to associate operator and rule for %s", file_name+4);
            IB_FTRACE_RET_STATUS(rc);
        }

        ib_log_debug(cp->ib, 1, "Set operator %s for rule %s",
                     file_name,
                     ib_rule_id(rule));

    }
    else {
        ib_log_error(cp->ib, 1, "RuleExt does not support rule type %s.",
            file_name);
        IB_FTRACE_RET_STATUS(IB_EINVAL);
    }

    /* Finally, register the rule */
    rc = ib_rule_register(cp->ib, cp->cur_ctx, rule, phase);

    if (rc != IB_OK) {
        ib_log_error(cp->ib, 1, "Error registering rule: %d", rc);
        IB_FTRACE_RET_STATUS(rc);
    }

    ib_log_debug(cp->ib, 1, "Registered rule %s", ib_rule_id(rule));

    /* Done */
    IB_FTRACE_RET_STATUS(IB_OK);
}

/**
 * @brief Parse a Rule directive.
 * @details Register a Rule directive to the engine.
 * @param[in,out] cp Configuration parser that contains the engine being
 *                configured.
 * @param[in] name The directive name.
 * @param[in] vars The list of variables passed to @c name.
 * @param[in] cbdata User data. Unused.
 */
static ib_status_t rules_rule_params(ib_cfgparser_t *cp,
                                     const char *name,
                                     const ib_list_t *vars,
                                     void *cbdata)
{
    IB_FTRACE_INIT();
    ib_status_t rc;
    const ib_list_node_t *inputs;
    const ib_list_node_t *op;
    const ib_list_node_t *mod;
    ib_rule_phase_t phase = PHASE_NONE;
    ib_rule_t *rule;

    if (cbdata != NULL) {
        IB_FTRACE_MSG("Callback data is not null.");
    }

    /* Get the inputs string */
    inputs = ib_list_first_const(vars);
    if ( (inputs == NULL) || (inputs->data == NULL) ) {
        ib_log_error(cp->ib, 1, "No inputs for rule");
        IB_FTRACE_RET_STATUS(IB_EINVAL);
    }

    /* Get the operator string */
    op = ib_list_node_next_const(inputs);
    if ( (op == NULL) || (op->data == NULL) ) {
        ib_log_error(cp->ib, 1, "No operator for rule");
        IB_FTRACE_RET_STATUS(IB_EINVAL);
    }

    /* Allocate a rule */
    rc = ib_rule_create(cp->ib, cp->cur_ctx, &rule);
    if (rc != IB_OK) {
        ib_log_error(cp->ib, 1, "Failed to allocate rule: %d", rc);
        IB_FTRACE_RET_STATUS(rc);
    }

    /* Parse the inputs */
    rc = parse_inputs(cp, rule, inputs->data);
    if (rc != IB_OK) {
        ib_log_error(cp->ib, 1,
                     "Error parsing rule inputs: %d", rc);
        IB_FTRACE_RET_STATUS(rc);
    }

    /* Parse the operator */
    rc = parse_operator(cp, rule, op->data);
    if (rc != IB_OK) {
        ib_log_error(cp->ib, 1,
                     "Error parsing rule inputs: %d", rc);
        IB_FTRACE_RET_STATUS(rc);
    }

    /* Parse all of the modifiers */
    mod = op;
    while( (mod = ib_list_node_next_const(mod)) != NULL) {
        rc = parse_modifier(cp, rule, &phase, mod->data);
        if (rc != IB_OK) {
            ib_log_error(cp->ib, 1,
               "Error parsing rule modifier - \"%s\".",
                mod->data);
            IB_FTRACE_RET_STATUS(rc);
        }
    }

    /* Finally, register the rule */
    rc = ib_rule_register(cp->ib, cp->cur_ctx, rule, phase);
    if (rc != IB_OK) {
        ib_log_error(cp->ib, 1, "Error registering rule: %d", rc);
        IB_FTRACE_RET_STATUS(rc);
    }

    /* Done */
    IB_FTRACE_RET_STATUS(IB_OK);
}


static IB_DIRMAP_INIT_STRUCTURE(rules_directive_map) = {

    /* Give the config parser a callback for the Rule and RuleExt directive */
    IB_DIRMAP_INIT_LIST(
        "Rule",
        rules_rule_params,
        NULL
    ),

    IB_DIRMAP_INIT_LIST(
        "RuleExt",
        rules_ruleext_params,
        NULL
    ),

    /* signal the end of the list */
    IB_DIRMAP_INIT_LAST
};

static void clean_up_ipc_mem(void)
{
    ib_lock_destroy(&g_lua_lock);
}

static ib_status_t rules_init(ib_engine_t *ib, ib_module_t *m)
{
    IB_FTRACE_INIT();

    /* Error code from Iron Bee calls. */
    ib_status_t ib_rc;
    ib_core_cfg_t *corecfg = NULL;

    const char* ffi_file_name = "ironbee-ffi.lua";
    char *ffi_file_path = NULL;

    ib_rc = ib_lock_init(&g_lua_lock);

    if (ib_rc != IB_OK) {
        ib_log_error(ib, 1, "Failed to initialize lua global lock.");
    }

    atexit(&clean_up_ipc_mem);

    ib_log_debug(ib, 1, "Initializing rules module.");

    if (m == NULL) {
        IB_FTRACE_MSG("Module is null.");
        clean_up_ipc_mem();
        IB_FTRACE_RET_STATUS(IB_EINVAL);
    }

    g_ironbee_rules_lua = luaL_newstate();

    if (g_ironbee_rules_lua == NULL) {
        ib_log_error(ib, 0, "Failed to create LuaJIT state.");
        IB_FTRACE_RET_STATUS(IB_EALLOC);
    }

    luaL_openlibs(g_ironbee_rules_lua);

    ib_rc = ib_context_module_config(ib_context_main(ib),
                                     ib_core_module(),
                                     (void *)&corecfg);

    if (ib_rc != IB_OK) {
        ib_log_error(ib, 1, "Could not retrieve core module configuration.");
        IB_FTRACE_RET_STATUS(ib_rc);
    }

    ffi_file_path = malloc(strlen(corecfg->module_base_path) +
                           strlen(ffi_file_name) +
                           2);

    if (ffi_file_path == NULL) {
        clean_up_ipc_mem();
        IB_FTRACE_RET_STATUS(IB_EALLOC);
    }

    /* Build string module_base_path/ffi_file_name. */
    strcpy(ffi_file_path, corecfg->module_base_path);
    strcpy(ffi_file_path + strlen(ffi_file_path), "/");
    strcpy(ffi_file_path + strlen(ffi_file_path), ffi_file_name);

    /* Load and evaluate the ffi file. */
    ib_rc = ib_lua_load_eval(ib, g_ironbee_rules_lua, ffi_file_path);

    if (ib_rc != IB_OK) {
        ib_log_error(ib, 1,
            "Failed to eval \"%s\" for Lua rule execution.",
            ffi_file_path);
        free(ffi_file_path);
        ffi_file_path = NULL;
        clean_up_ipc_mem();
        IB_FTRACE_RET_STATUS(ib_rc);
    }

    /* Require the ironbee module we just evaled. */
    ib_rc = ib_lua_require(ib, g_ironbee_rules_lua, "ironbee", "ironbee-ffi");
    if (ib_rc != IB_OK) {
        ib_log_error(ib, 1,
            "Failed to require \"%s\" for Lua rule execution.",
            ffi_file_path);
        free(ffi_file_path);
        ffi_file_path = NULL;
        clean_up_ipc_mem();
        IB_FTRACE_RET_STATUS(ib_rc);
    }

    free(ffi_file_path);
    ffi_file_path = NULL;

    /* Require the ffi module. */
    ib_rc = ib_lua_require(ib, g_ironbee_rules_lua, "ffi", "ffi");
    if (ib_rc != IB_OK) {
        ib_log_error(ib, 1, "Failed to load FFI for Lua rule execution.");
        clean_up_ipc_mem();
        IB_FTRACE_RET_STATUS(ib_rc);
    }

    IB_FTRACE_RET_STATUS(IB_OK);
}

static ib_status_t rules_fini(ib_engine_t *ib, ib_module_t *m)
{
    IB_FTRACE_INIT();
    ib_log_debug(ib, 4, "Rules module unloading.");

    ib_lock_destroy(&g_lua_lock);

    if (g_ironbee_rules_lua != NULL) {
        lua_close(g_ironbee_rules_lua);
        g_ironbee_rules_lua = NULL;
    }

    IB_FTRACE_RET_STATUS(IB_OK);
}

/* Initialize the module structure. */
IB_MODULE_INIT(
    IB_MODULE_HEADER_DEFAULTS,           /* Default metadata */
    MODULE_NAME_STR,                     /* Module name */
    IB_MODULE_CONFIG_NULL,               /* Global config data */
    NULL,                                /* Configuration field map */
    rules_directive_map,                 /* Config directive map */
    rules_init,                          /* Initialize function */
    rules_fini,                          /* Finish function */
    NULL,                                /* Context init function */
    NULL                                 /* Context fini function */
);

