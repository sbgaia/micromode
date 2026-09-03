#include "micromode.h"
#include "reactor-uc/environment.h"
#include "unity.h"

Environment* _lf_environment = NULL;

void setUp(void) {}
void tearDown(void) {}

/** The library was compiled and linked if this symbol resolves and answers LF_OK. */
void test_abi_check(void) { TEST_ASSERT_EQUAL(LF_OK, lf_micromode_abi_check()); }

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_abi_check);
  return UNITY_END();
}
