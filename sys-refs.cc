#include <stdio.h>
#include <sys/types.h>
#include <getopt.h>

#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include <iostream>

#include "lib/debug.h"
#include "Option.h"
#include "ProcMaps.h"
#include "ProcIdlePages.h"
#include "Migration.h"
#include "GlobalScan.h"

using namespace std;

Option option;

int debug_level()
{
  return option.debug_level;
}

static const struct option opts[] = {
  {"pid",       required_argument,  NULL, 'p'},
  {"interval",  required_argument,  NULL, 'i'},
  {"sleep",     required_argument,  NULL, 's'},
  {"loop",      required_argument,  NULL, 'l'},
  {"output",    required_argument,  NULL, 'o'},
  {"dram",      required_argument,  NULL, 'd'},
  {"migrate",   required_argument,  NULL, 'm'},
  {"verbose",   required_argument,  NULL, 'v'},
  {"help",      no_argument,        NULL, 'h'},
  {NULL,        0,                  NULL, 0}
};

static void usage(char *prog)
{
  fprintf(stderr,
          "%s [option] ...\n"
          "    -h|--help       Show this information\n"
          "    -p|--pid        The PID to scan\n"
          "    -i|--interval   The scan interval in seconds\n"
          "    -s|--sleep      Seconds to sleep between scan rounds\n"
          "    -l|--loop       The number of scan rounds\n"
          "    -o|--output     The output file, defaults to refs-count-PID\n"
          "    -d|--dram       The DRAM percent, wrt. DRAM+PMEM total size\n"
          "    -m|--migrate    Migrate what: 0|none, 1|hot, 2|cold, 3|both\n"
          "    -v|--verbose    Show debug info\n",
          prog);

  exit(0);
}

static void parse_cmdline(int argc, char *argv[])
{
  int options_index = 0;
	int opt = 0;
	const char *optstr = "hvp:i:s:l:o:d:m:";

  while ((opt = getopt_long(argc, argv, optstr, opts, &options_index)) != EOF) {
    switch (opt) {
    case 0:
      break;
    case 'p':
      option.pid = atoi(optarg);
      break;
    case 's':
      option.sleep_secs = atof(optarg);
      break;
    case 'i':
      option.interval = atof(optarg);
      break;
    case 'l':
      option.nr_loops = atoi(optarg);
      break;
    case 'o':
      option.output_file = optarg;
      break;
    case 'd':
      option.dram_percent = atoi(optarg);
      break;
    case 'm':
      option.migrate_what = Migration::parse_migrate_name(optarg);
      break;
    case 'v':
      ++option.debug_level;
      break;
    case 'h':
    case '?':
    default:
      usage(argv[0]);
    }
  }
}

int main(int argc, char *argv[])
{
  setlocale(LC_NUMERIC, "");

  parse_cmdline(argc, argv);

  GlobalScan gscan;

  gscan.main_loop();

  return 0;
}
