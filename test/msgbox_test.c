// msgbox_test.c
//
// Unit tests for msgbox.
//

// TODO
// * Make it possible to easily run a test with one
//   part running on a remote server, and another
//   running on a client, possibly behind a NAT.
//

#include "ctest.h"

int null_test() {
  return test_success;
}

int main(int argc, char **argv) {
  start_all_tests(argv[0]);
  run_tests(null_test);
  return end_all_tests();
}
