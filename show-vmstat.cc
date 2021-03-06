/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (c) 2018 Intel Corporation
 *
 * Authors: Fengguang Wu <fengguang.wu@intel.com>
 */

#include <iostream>

#include "ProcVmstat.h"

int main(int argc, char *argv[])
{
  ProcVmstat vmstat;

  vmstat.load_vmstat();
  vmstat.load_numa_vmstat();
  std::cout << "nr_free_pages "   << vmstat.vmstat("nr_free_pages")    << std::endl;
  std::cout << "nr_free_pages 0 " << vmstat.vmstat(0, "nr_free_pages") << std::endl;
  return 0;
}
