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
 ****************************************************************************/

/**
 * @file
 * @brief Predicate --- Standard ValueList Tests
 *
 * @author Christopher Alfeld <calfeld@qualys.com>
 **/

#include <predicate/standard_valuelist.hpp>
#include "standard_test.hpp"

using namespace IronBee::Predicate;
using namespace std;

class TestStandardValueList :
    public StandardTest
{
};

TEST_F(TestStandardValueList, Name)
{
    EXPECT_TRUE(eval_bool("(setName 'a' 'b')"));
    EXPECT_EQ("b", eval_s("(setName 'a' 'b')"));
    EXPECT_THROW(eval_bool("(setName)"), IronBee::einval);
    EXPECT_THROW(eval_bool("(setName null 'a')"), IronBee::einval);
    EXPECT_THROW(eval_bool("(setName 'a')"), IronBee::einval);
    EXPECT_THROW(eval_bool("(setName 'a' 'b' 'c')"), IronBee::einval);
}