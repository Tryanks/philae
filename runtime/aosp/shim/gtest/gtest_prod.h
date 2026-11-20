// AOSP headers befriend their unit tests through googletest; the runtime ships no tests.
#pragma once
#define FRIEND_TEST(test_case_name, test_name)
