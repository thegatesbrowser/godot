// Stub for gtest_prod.h.
// We don't build chromium tests; gtest's FRIEND_TEST macro becomes a no-op.
#ifndef GOOGLETEST_INCLUDE_GTEST_GTEST_PROD_H_
#define GOOGLETEST_INCLUDE_GTEST_GTEST_PROD_H_

#define FRIEND_TEST(test_case_name, test_name) friend class test_case_name##_##test_name##_Test

#endif  // GOOGLETEST_INCLUDE_GTEST_GTEST_PROD_H_
