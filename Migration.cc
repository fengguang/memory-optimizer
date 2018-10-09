#include <stdio.h>
#include <ctype.h>

#include <map>
#include <string>
#include <iostream>
#include <algorithm>
#include <sys/mman.h>

#include <numa.h>
#include <numaif.h>
#include "Option.h"
#include "Migration.h"
#include "lib/debug.h"
#include "lib/stats.h"
#include "AddrSequence.h"

#define MPOL_MF_SW_YOUNG (1<<7)

using namespace std;

std::unordered_map<std::string, MigrateWhat> Migration::migrate_name_map = {
	    {"none", MIGRATE_NONE},
	    {"hot",  MIGRATE_HOT},
	    {"cold", MIGRATE_COLD},
	    {"both", MIGRATE_BOTH},
};

Migration::Migration(pid_t n)
  : ProcIdlePages(n)
{
  migrate_target_node.resize(PMD_ACCESSED + 1);
  migrate_target_node[PTE_IDLE]      = Option::PMEM_NUMA_NODE;
  migrate_target_node[PTE_ACCESSED]  = Option::DRAM_NUMA_NODE;

  migrate_target_node[PMD_IDLE]      = Option::PMEM_NUMA_NODE;
  migrate_target_node[PMD_ACCESSED]  = Option::DRAM_NUMA_NODE;
}

MigrateWhat Migration::parse_migrate_name(std::string name)
{
  if (isdigit(name[0])) {
    int m = atoi(name.c_str());
    if (m <= MIGRATE_BOTH)
      return (MigrateWhat)m;
    cerr << "invalid migrate type: " << name << endl;
    return MIGRATE_NONE;
  }

  auto search = migrate_name_map.find(name);

  if (search != migrate_name_map.end())
    return search->second;

  cerr << "invalid migrate type: " << name << endl;
  return MIGRATE_NONE;
}

size_t Migration::get_threshold_refs(ProcIdlePageType type,
                                     int& min_refs, int& max_refs)
{
  int nr_walks = get_nr_walks();

  if (type & PAGE_ACCESSED_MASK && option.nr_walks == 0) {
    min_refs = nr_walks;
    max_refs = nr_walks;
    return 0;
  }
  if (type & PAGE_ACCESSED_MASK && option.hot_min_refs > 0) {
    min_refs = option.hot_min_refs;
    max_refs = nr_walks;
    return 0;
  }
  if (!(type & PAGE_ACCESSED_MASK) && option.cold_max_refs >= 0) {
    min_refs = 0;
    max_refs = option.cold_max_refs;
    return 0;
  }

  const AddrSequence& page_refs = get_pagetype_refs(type).page_refs;
  vector<unsigned long> refs_count = get_pagetype_refs(type).refs_count;

  double ratio;

  if (option.dram_percent) {
    if (migrate_target_node[type] == Option::DRAM_NUMA_NODE)
      ratio = option.dram_percent / 100.0;
    else
      ratio = (100.0 - option.dram_percent) / 100.0;
  } else {
    ProcVmstat proc_vmstat;
    ratio = (double) proc_vmstat.anon_capacity(migrate_target_node[type]) / proc_vmstat.anon_capacity();
  }

  // XXX: this assumes all processes have same hot/cold distribution
  size_t portion = page_refs.size() * ratio;
  long quota = portion;

  fmt.print("migrate ratio: %.2f = %lu / %lu\n", ratio, portion, page_refs.size());

  if (type & PAGE_ACCESSED_MASK) {
    min_refs = nr_walks;
    max_refs = nr_walks;
    for (; min_refs > 1; --min_refs) {
      quota -= refs_count[min_refs];
      if (quota <= 0)
        break;
    }
    if (min_refs < nr_walks)
      ++min_refs;
  } else {
    min_refs = 0;
    max_refs = 0;
    for (; max_refs < nr_walks / 2; ++max_refs) {
      quota -= refs_count[max_refs];
      if (quota <= 0)
        break;
    }
    max_refs >>= 1;
  }

  fmt.print("refs range: %d-%d\n", min_refs, max_refs);

  return portion;
}

int Migration::select_top_pages(ProcIdlePageType type)
{
  AddrSequence& page_refs = get_pagetype_refs(type).page_refs;
  int min_refs;
  int max_refs;
  unsigned long addr;
  uint8_t ref_count;
  int iter_ret;

  if (page_refs.empty())
    return 1;

  get_threshold_refs(type, min_refs, max_refs);

  /*
  for (auto it = page_refs.begin(); it != page_refs.end(); ++it) {
    printdd("va: %lx count: %d\n", it->first, (int)it->second);
    if (it->second >= min_refs &&
        it->second <= max_refs)
      pages_addr[type].push_back((void *)(it->first << PAGE_SHIFT));
  }
  */

  iter_ret = page_refs.get_first(addr, ref_count);
  while (!iter_ret) {
    if (ref_count >= min_refs &&
        ref_count <= max_refs)
      pages_addr[type].push_back((void *)addr);

    iter_ret = page_refs.get_next(addr, ref_count);
  }

  if (pages_addr[type].empty())
    return 1;

  sort(pages_addr[type].begin(), pages_addr[type].end());

  if (debug_level() >= 2)
    for (size_t i = 0; i < pages_addr[type].size(); ++i) {
      cout << "page " << i << ": " << pages_addr[type][i] << endl;
    }

  return 0;
}

int Migration::migrate()
{
  int err = 0;

  fmt.clear();
  fmt.reserve(1<<10);

  if (option.migrate_what & MIGRATE_COLD) {
    err = migrate(PTE_IDLE);
    if (err)
      goto out;
    err = migrate(PMD_IDLE);
    if (err)
      goto out;
  }

  if (option.migrate_what & MIGRATE_HOT) {
    err = migrate(PTE_ACCESSED);
    if (err)
      goto out;
    err = migrate(PMD_ACCESSED);
  }

out:
  if (!fmt.empty())
    std::cout << fmt.str();

  return err;
}

int Migration::migrate(ProcIdlePageType type)
{
  int ret;

  ret = select_top_pages(type);
  if (ret)
    return std::min(ret, 0);

  ret = do_move_pages(type);
  return ret;
}

long Migration::__move_pages(pid_t pid, unsigned long nr_pages,
                             void **addrs, int node)
{
  std::vector<int> nodes;
  long ret = 0;

  migrate_status.resize(nr_pages);

  unsigned long batch_size = 1 << 12;
  for (unsigned long i = 0; i < nr_pages; i += batch_size) {
    unsigned long size = min(batch_size, nr_pages - i);
    nodes.resize(size, node);
    ret = move_pages(pid,
                     size,
                     addrs + i,
                     &nodes[0],
                     &migrate_status[i], MPOL_MF_MOVE | MPOL_MF_SW_YOUNG);
    if (ret) {
      perror("move_pages");
      break;
    }
  }

  return ret;
}

long Migration::do_move_pages(ProcIdlePageType type)
{
  auto& addrs = pages_addr[type];
  long nr_pages = addrs.size();
  long ret;

  ret = __move_pages(pid, nr_pages, &addrs[0], migrate_target_node[type]);

  return ret;
}

std::unordered_map<int, int> Migration::calc_migrate_stats()
{
  std::unordered_map<int, int> stats;

  for (int &i : migrate_status)
    inc_count(stats, i);

  return stats;
}

void Migration::show_numa_stats()
{
  ProcVmstat proc_vmstat;

  proc_vmstat.load_vmstat();
  proc_vmstat.load_numa_vmstat();

  const auto& numa_vmstat = proc_vmstat.get_numa_vmstat();
  unsigned long total_anon_kb = proc_vmstat.vmstat("nr_inactive_anon") +
                                proc_vmstat.vmstat("nr_active_anon") +
                                proc_vmstat.vmstat("nr_isolated_anon");

  total_anon_kb *= PAGE_SIZE >> 10;
  printf("%'15lu       anon total\n", total_anon_kb);

  int nid = 0;
  for (auto& map: numa_vmstat) {
    unsigned long anon_kb = map.at("nr_inactive_anon") +
                            map.at("nr_active_anon") +
                            map.at("nr_isolated_anon");
    anon_kb *= PAGE_SIZE >> 10;
    printf("%'15lu  %2d%%  anon node %d\n", anon_kb, percent(anon_kb, total_anon_kb), nid);
    ++nid;
  }
}

void Migration::fill_addrs(std::vector<void *>& addrs, unsigned long start)
{
    void **p = &addrs[0];
    void **pp = &addrs[addrs.size()-1];

    for (; p <= pp; ++p) {
      *p = (void *)start;
      start += PAGE_SIZE;
    }
}

void Migration::dump_node_percent()
{
  auto stats = calc_migrate_stats();
  size_t nr_node0 = (size_t)stats[0];
  size_t nr_err = 0;

  for (auto &kv : stats)
  {
    int status = kv.first;

    if (status < 0)
      ++nr_err;
  }

  fmt.print("%3u ", percent(nr_node0, migrate_status.size()));
  if (nr_err)
    fmt.print("(-%u) ", percent(nr_err, migrate_status.size()));
}

int Migration::dump_vma_nodes(proc_maps_entry& vma)
{
  unsigned long nr_pages;
  int err = 0;

  if (vma.end - vma.start < 1<<30)
    return 0;

  nr_pages = (vma.end - vma.start) >> PAGE_SHIFT;

  unsigned long total_kb = (vma.end - vma.start) >> 10;
  fmt.print("VMA size: %'15lu \nN0 percent:", total_kb);

  const int nr_slots = 10;
  unsigned long slot_pages = nr_pages / nr_slots;

  std::vector<void *> addrs;
  addrs.resize(slot_pages);
  migrate_status.resize(slot_pages);

  for (int i = 0; i < nr_slots; ++i)
  {
    fill_addrs(addrs, vma.start + i * addrs.size() * PAGE_SIZE);

    err = move_pages(pid,
                     slot_pages,
                     &addrs[0],
                     NULL,
                     &migrate_status[0], MPOL_MF_MOVE);
    if (err) {
      perror("move_pages");
      return err;
    }

    dump_node_percent();
  }

  return err;
}

int Migration::dump_task_nodes()
{
  ProcMaps proc_maps;
  int err = 0;

  auto maps = proc_maps.load(pid);

  for (auto &vma: maps) {
    err = dump_vma_nodes(vma);
    if (err)
      break;
  }

  return err;
}

