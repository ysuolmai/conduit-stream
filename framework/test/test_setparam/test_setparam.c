// Host unit tests for the pure SET_PARAMETER Content-Type dispatch decision.
#include <unity.h>
#include "setparam.c"
void setUp(void){} void tearDown(void){}

void test_text_parameters(void){
    TEST_ASSERT_EQUAL_INT(SETPARAM_VOLUME, setparam_classify("text/parameters"));
}
void test_dmap(void){
    TEST_ASSERT_EQUAL_INT(SETPARAM_METADATA, setparam_classify("application/x-dmap-tagged"));
}
void test_case_insensitive(void){
    TEST_ASSERT_EQUAL_INT(SETPARAM_VOLUME, setparam_classify("Text/Parameters"));
}
void test_trailing_params_ignored(void){
    TEST_ASSERT_EQUAL_INT(SETPARAM_VOLUME, setparam_classify("text/parameters; charset=utf-8"));
}
void test_leading_ws(void){
    TEST_ASSERT_EQUAL_INT(SETPARAM_METADATA, setparam_classify("  application/x-dmap-tagged"));
}
void test_image_is_other(void){
    TEST_ASSERT_EQUAL_INT(SETPARAM_OTHER, setparam_classify("image/jpeg"));
}
void test_null_is_other(void){
    TEST_ASSERT_EQUAL_INT(SETPARAM_OTHER, setparam_classify(NULL));
}
void test_prefix_not_matched(void){        // "text/parametersX" must not match
    TEST_ASSERT_EQUAL_INT(SETPARAM_OTHER, setparam_classify("text/parametersX"));
}
int main(void){ UNITY_BEGIN();
    RUN_TEST(test_text_parameters); RUN_TEST(test_dmap); RUN_TEST(test_case_insensitive);
    RUN_TEST(test_trailing_params_ignored); RUN_TEST(test_leading_ws);
    RUN_TEST(test_image_is_other); RUN_TEST(test_null_is_other);
    RUN_TEST(test_prefix_not_matched);
    return UNITY_END(); }
