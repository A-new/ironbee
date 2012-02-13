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

#ifndef _IB_RULE_DEFS_H_
#define _IB_RULE_DEFS_H_

/**
 * @file
 * @brief IronBee - Rule engine definitions
 *
 * @author Nick LeRoy <nleroy@qualys.com>
 */

#include <ironbee/build.h>
#include <ironbee/release.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup IronBeeRuleDefs Rule Engine Definitions
 * @ingroup IronBeeEngine
 * @{
 */

/**
 * Rule phase number.
 */
typedef enum {
    PHASE_INVALID = -1,             /**< Invalid; used to terminate list */
    PHASE_NONE,                     /**< No phase */
    PHASE_REQUEST_HEADER,           /**< Request header available. */
    PHASE_REQUEST_BODY,             /**< Request body available. */
    PHASE_RESPONSE_HEADER,          /**< Response header available. */
    PHASE_RESPONSE_BODY,            /**< Response body available. */
    PHASE_POSTPROCESS,              /**< Post-processing phase. */
    PHASE_MAX = PHASE_POSTPROCESS,  /**< Max phase number. */
    IB_RULE_PHASE_COUNT,            /**< Size of rule phase lists */
} ib_rule_phase_t;

/**
 * Rule flags
 *
 * If the external flag is set, the rule engine will always execute the
 * operator, passing NULL in as the field pointer.  The external rule is
 * expected to extract whatever fields, etc. it requires itself.
 */
#define IB_RULE_FLAG_NONE       (0x0)     /**< No flags */
#define IB_RULE_FLAG_EXTERNAL   (1 << 0)  /**< External rule */
#define IB_RULE_FLAG_CHAIN      (1 << 1)  /**< Rule is part of a chain */
#define IB_RULE_FLAG_CHAINED_TO (1 << 2)  /**< Rule is target of a chain */

/**
 * Rule engine: Basic rule type
 */
typedef struct ib_rule_t ib_rule_t;


/**
 * @} IronBeeRuleDefs
 */

#ifdef __cplusplus
}
#endif

#endif /* _IB_RULE_DEFS_H_ */
