#include "test/birdtest.h"
#include "test/bt-utils.h"

#include <stdio.h>








int
main(int argc, char *argv[])
{
  bt_init(argc, argv);

  printf("%d\n", argc);

  for (int i = 0; i < argc; i++) {
    printf("%s\n", argv[i]);
  }

  return bt_exit_value();
}