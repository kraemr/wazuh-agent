/*
 * Copyright (C) 2015-2019, Wazuh Inc.
 *
 * This program is free software; you can redistribute it
 * and/or modify it under the terms of the GNU General Public
 * License (version 2) as published by the FSF - Free Software
 * Foundation.
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <stdio.h>
#include <string.h>

#include "../wazuh_db/wdb.h"
#include "../headers/shared.h"

static const char* VALID_ENTRY = "{"
    "\"path\": \"/test\",\n"
    "\"timestamp\": 10,\n"
    "\"attributes\": {}\n"
    "}"
;

/* setup/teardown */
static int setup_wdb_t(void **state) {
    wdb_t *data = calloc(1, sizeof(wdb_t));

    if(!data) {
        return -1;
    }

    *state = data;
    return 0;
}

static int teardown_wdb_t(void **state) {
    wdb_t *data = *state;

    if(data) {
        os_free(data->agent_id);
        os_free(data);
    }

    return 0;
}

extern cJSON* __real_cJSON_Parse(const char * item);

/* redefinitons/wrapping */

int __wrap_wdb_begin2(wdb_t* aux) 
{
    return mock();
}

cJSON* __wrap_cJSON_Parse(const char * item) {	
    return mock_type(cJSON*);	
}	

int __wrap_cJSON_Delete(cJSON* item) {	
    return 0;	
}

void __wrap__merror(const char * file, int line, const char * func, const char *msg, ...) {
    char formatted_msg[OS_MAXSTR];
    va_list args;

    va_start(args, msg);
    vsnprintf(formatted_msg, OS_MAXSTR, msg, args);
    va_end(args);

    check_expected(formatted_msg);
}

void __wrap__mdebug1(const char * file, int line, const char * func, const char *msg, ...)
{
    char formatted_msg[OS_MAXSTR];
    va_list args;

    va_start(args, msg);
    vsnprintf(formatted_msg, OS_MAXSTR, msg, args);
    va_end(args);

    check_expected(formatted_msg);
}

char* __wrap_cJSON_GetStringValue(cJSON * item)	
{	
    return mock_type(char*);	
}	

cJSON_bool __wrap_cJSON_IsNumber(cJSON * item)	
{	
    return mock_type(cJSON_bool);	
}	

cJSON_bool __wrap_cJSON_IsObject(cJSON * item)	
{	
    return mock_type(cJSON_bool);	
}	

int __wrap_wdb_stmt_cache(wdb_t wdb, int index)
{
    return mock();
}

int __wrap_sqlite3_bind_text()	
{	
    return mock();	
}	

int __wrap_sqlite3_bind_int64()	
{	
    return mock();	
}

int __wrap_sqlite3_step()
{
    return mock();
}

/* tests */

static void test_wdb_syscheck_save2_wbs_null(void **state)
{
    (void) state; /* unused */
    int ret;
    will_return(__wrap_cJSON_Parse, cJSON_CreateObject());
    expect_string(__wrap__merror, formatted_msg, "WDB object cannot be null.");
    ret = wdb_syscheck_save2(NULL, "{}");
    assert_int_equal(ret, -1);
    
}

static void test_wdb_syscheck_save2_payload_null(void **state)
{
    int ret;

    wdb_t * data = *state;
    data->agent_id = strdup("000");
    will_return(__wrap_cJSON_Parse, NULL);
    expect_string(__wrap__mdebug1, formatted_msg, "DB(000): cannot parse FIM payload: '(null)'");
    ret = wdb_syscheck_save2(data, NULL);
    assert_int_equal(ret, -1);
}

static void test_wdb_syscheck_save2_data_null(void **state)
{
    int ret;

    wdb_t * data = *state;
    data->agent_id = strdup("000");
    will_return(__wrap_cJSON_Parse, cJSON_CreateObject());
    will_return(__wrap_wdb_begin2, 0);
    expect_string(__wrap__merror, formatted_msg, "DB(000) fim/save request with no file path argument.");
    expect_string(__wrap__mdebug1, formatted_msg, "DB(000) Can't insert file entry.");
    ret = wdb_syscheck_save2(data, "{}");
    assert_int_equal(ret, -1);
    
}

static void test_wdb_syscheck_save2_fail_transaction(void **state)
{
    int ret;

    wdb_t * data = *state;
    data->agent_id = strdup("000");
    data->transaction = 0;
    cJSON * doc = cJSON_CreateObject();	
    will_return(__wrap_cJSON_Parse, doc);
    will_return(__wrap_wdb_begin2, -1);
    expect_string(__wrap__merror, formatted_msg, "DB(000) Can't begin transaction.");
    ret = wdb_syscheck_save2(data, "{}");
    cJSON_Delete(doc);
    assert_int_equal(ret, -1);
}

static void test_wdb_syscheck_save2_fail_file_entry(void **state)
{
    int ret;

    wdb_t * data = *state;
    data->agent_id = strdup("000");
    data->transaction = 1;
    cJSON * doc = cJSON_CreateObject();
    will_return(__wrap_cJSON_Parse, doc);
    expect_string(__wrap__merror, formatted_msg, "DB(000) fim/save request with no file path argument.");
    expect_string(__wrap__mdebug1, formatted_msg, "DB(000) Can't insert file entry.");
    const char *entry = 
    "{"
    "\"path\": \"/test\",\n"
    "\"timestamp\": \"string-val\"\n"
    "}"
    ;
    ret = wdb_syscheck_save2(data, entry);
    cJSON_Delete(doc);
    assert_int_equal(ret, -1);
}


static void test_wdb_syscheck_save2_success(void **state)
{
    int ret;

    wdb_t * data = *state;
    data->agent_id = strdup("000");
    data->transaction = 1;
    cJSON * doc = __real_cJSON_Parse(VALID_ENTRY);	 
    will_return(__wrap_cJSON_Parse, doc);	
    will_return(__wrap_cJSON_GetStringValue, "/test");	
    will_return(__wrap_cJSON_IsNumber, true);	
    will_return(__wrap_cJSON_IsObject, true);	
    will_return(__wrap_wdb_stmt_cache, 1);
    will_return(__wrap_sqlite3_bind_text,1);	
    will_return(__wrap_sqlite3_bind_int64,0);
    will_return(__wrap_sqlite3_step,101);
    ret = wdb_syscheck_save2(data, VALID_ENTRY);
    cJSON_Delete(doc);
    assert_int_equal(ret, 0);
}


static void test_wdb_fim_insert_entry2_wdb_null(void **state)
{
    (void) state; /* unused */
    int ret;
    expect_string(__wrap__merror, formatted_msg, "WDB object cannot be null.");
    ret = wdb_fim_insert_entry2(NULL, __real_cJSON_Parse(VALID_ENTRY));
    assert_int_equal(ret, -1);    
}

static void test_wdb_fim_insert_entry2_data_null(void **state)
{
    int ret;

    wdb_t * data = *state;
    data->agent_id = strdup("000");
    expect_string(__wrap__merror, formatted_msg, "DB(000) fim/save request with no file path argument.");
    ret = wdb_fim_insert_entry2(data,NULL);
    assert_int_equal(ret, -1);    
}

static void test_wdb_fim_insert_entry2_path_null(void **state)
{
    int ret;

    wdb_t * data = *state;
    data->agent_id = strdup("000");
    cJSON* doc = cJSON_CreateObject();	
    expect_string(__wrap__merror, formatted_msg, "DB(000) fim/save request with no file path argument.");
    ret = wdb_fim_insert_entry2(data, doc);
    cJSON_Delete(doc);
    assert_int_equal(ret, -1);    
}

static void test_wdb_fim_insert_entry2_timestamp_null(void **state)
{
    int ret;
    cJSON* doc;

    wdb_t * data = *state;
    data->agent_id = strdup("000");
    doc = __real_cJSON_Parse(VALID_ENTRY);
    will_return(__wrap_cJSON_GetStringValue, "/test");	
    will_return(__wrap_cJSON_IsNumber, false);
    cJSON_ReplaceItemInObject(doc, "timestamp", cJSON_CreateString(""));
    expect_string(__wrap__merror, formatted_msg, "DB(000) fim/save request with no timestamp path argument.");
    ret = wdb_fim_insert_entry2(data, doc);
    cJSON_Delete(doc);
    assert_int_equal(ret, -1);  
}

static void test_wdb_fim_insert_entry2_attributes_null(void **state)
{
    int ret;
    cJSON* doc;

    wdb_t * data = *state;
    data->agent_id = strdup("000");
    doc = __real_cJSON_Parse(VALID_ENTRY);
    will_return(__wrap_cJSON_GetStringValue, "/test");	
    will_return(__wrap_cJSON_IsNumber, true);
    will_return(__wrap_cJSON_IsObject, false);
    cJSON_ReplaceItemInObject(doc, "attributes", cJSON_CreateString(""));
    expect_string(__wrap__merror, formatted_msg, "DB(000) fim/save request with no valid attributes.");
    ret = wdb_fim_insert_entry2(data, doc);
    cJSON_Delete(doc);
    assert_int_equal(ret, -1);
}

static void test_wdb_fim_insert_entry2_fail_cache(void **state)
{
    int ret;

    wdb_t * data = *state;
    data->agent_id = strdup("000");
    will_return(__wrap_cJSON_GetStringValue, "/test");	
    will_return(__wrap_cJSON_IsNumber, true);
    will_return(__wrap_cJSON_IsObject, true);
    will_return(__wrap_wdb_stmt_cache, -1);
    cJSON *doc = __real_cJSON_Parse(VALID_ENTRY);
    expect_string(__wrap__merror, formatted_msg, "DB(000) Can't cache statement");
    ret = wdb_fim_insert_entry2(data, doc);
    cJSON_Delete(doc);
    assert_int_equal(ret, -1);
}

static void test_wdb_fim_insert_entry2_fail_element_string(void **state)
{
    int ret;

    wdb_t * data = *state;
    data->agent_id = strdup("000");
    cJSON* doc = __real_cJSON_Parse(VALID_ENTRY);
    cJSON *array = cJSON_CreateObject();
    cJSON_AddItemToObject(array, "invalid_attribute", cJSON_CreateString("sasssss"));
    cJSON_ReplaceItemInObject(doc, "attributes", array);
    will_return(__wrap_cJSON_GetStringValue, "/test");	
    will_return(__wrap_cJSON_IsNumber, true);
    will_return(__wrap_cJSON_IsObject, true);
    will_return(__wrap_wdb_stmt_cache, 1);
    will_return(__wrap_sqlite3_bind_text, 1);
    will_return(__wrap_sqlite3_bind_int64,0);
    expect_string(__wrap__merror, formatted_msg, "DB(000) Invalid attribute name: invalid_attribute");
    ret = wdb_fim_insert_entry2(data, doc);
    cJSON_Delete(doc);
    assert_int_equal(ret, -1);
}

static void test_wdb_fim_insert_entry2_fail_sqlite3_stmt(void **state)
{
    int ret;

    wdb_t * data = *state;
    data->agent_id = strdup("000");
    will_return(__wrap_cJSON_GetStringValue, "/test");	
    will_return(__wrap_cJSON_IsNumber, true);
    will_return(__wrap_cJSON_IsObject, true);
    will_return(__wrap_wdb_stmt_cache, 1);
    will_return(__wrap_sqlite3_bind_text, 1);
    will_return(__wrap_sqlite3_bind_int64,0);
    will_return(__wrap_sqlite3_step,0);
    expect_string(__wrap__mdebug1, formatted_msg, "DB(000) sqlite3_step(): out of memory");
    cJSON* doc = __real_cJSON_Parse(VALID_ENTRY);
    ret = wdb_fim_insert_entry2(data, doc);
    cJSON_Delete(doc);
    assert_int_equal(ret, -1);
}

static void test_wdb_fim_insert_entry2_success(void **state)
{
    int ret;

    wdb_t * data = *state;
    data->agent_id = strdup("000");
    cJSON* doc = __real_cJSON_Parse(VALID_ENTRY);
    cJSON *array = cJSON_CreateObject();
    cJSON_AddItemToObject(array, "type", cJSON_CreateString("test_type"));
    cJSON_AddItemToObject(array, "uid", cJSON_CreateString("00000"));
    cJSON_AddItemToObject(array, "size", cJSON_CreateNumber(2048));
    cJSON_ReplaceItemInObject(doc, "attributes", array);
    will_return(__wrap_cJSON_GetStringValue, "/test");	
    will_return(__wrap_cJSON_IsNumber, true);
    will_return(__wrap_cJSON_IsObject, true);
    will_return(__wrap_wdb_stmt_cache, 1);
    will_return(__wrap_sqlite3_bind_text, 1);
    will_return(__wrap_sqlite3_bind_int64,0);
    will_return(__wrap_sqlite3_step,SQLITE_DONE);  
    will_return(__wrap_sqlite3_bind_text, 1);
    will_return(__wrap_sqlite3_bind_text, 1);
    ret = wdb_fim_insert_entry2(data, doc);
    cJSON_Delete(doc);
    assert_int_equal(ret, 0);
}

int main(void) {
    const struct CMUnitTest tests[] = {           
        //Test wdb_syscheck_save2
        cmocka_unit_test_setup_teardown(test_wdb_syscheck_save2_wbs_null, setup_wdb_t, teardown_wdb_t),
        cmocka_unit_test_setup_teardown(test_wdb_syscheck_save2_payload_null, setup_wdb_t, teardown_wdb_t),
        cmocka_unit_test_setup_teardown(test_wdb_syscheck_save2_data_null, setup_wdb_t, teardown_wdb_t),
        cmocka_unit_test_setup_teardown(test_wdb_syscheck_save2_fail_transaction, setup_wdb_t, teardown_wdb_t),
        cmocka_unit_test_setup_teardown(test_wdb_syscheck_save2_fail_file_entry, setup_wdb_t, teardown_wdb_t),
        cmocka_unit_test_setup_teardown(test_wdb_syscheck_save2_success, setup_wdb_t, teardown_wdb_t),

        //Test wdb_fim_insert_entry2
        cmocka_unit_test_setup_teardown(test_wdb_fim_insert_entry2_wdb_null, setup_wdb_t, teardown_wdb_t),
        cmocka_unit_test_setup_teardown(test_wdb_fim_insert_entry2_data_null, setup_wdb_t, teardown_wdb_t),
        cmocka_unit_test_setup_teardown(test_wdb_fim_insert_entry2_path_null, setup_wdb_t, teardown_wdb_t),
        cmocka_unit_test_setup_teardown(test_wdb_fim_insert_entry2_timestamp_null, setup_wdb_t, teardown_wdb_t),
        cmocka_unit_test_setup_teardown(test_wdb_fim_insert_entry2_attributes_null, setup_wdb_t, teardown_wdb_t),
        cmocka_unit_test_setup_teardown(test_wdb_fim_insert_entry2_fail_cache, setup_wdb_t, teardown_wdb_t),
        cmocka_unit_test_setup_teardown(test_wdb_fim_insert_entry2_fail_element_string, setup_wdb_t, teardown_wdb_t),
        cmocka_unit_test_setup_teardown(test_wdb_fim_insert_entry2_fail_sqlite3_stmt, setup_wdb_t, teardown_wdb_t),
        cmocka_unit_test_setup_teardown(test_wdb_fim_insert_entry2_success, setup_wdb_t, teardown_wdb_t),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
