#!/usr/bin/ruby

require 'yaml'
require_relative "ProcVmstat"
require_relative "ProcStatus"

# Basic test scheme:
#
# baseline run:
#         - run qemu on interleaved DRAM+PMEM nodes
#         - run workload in qemu
#
# migrate run:
#         - run qemu on interleaved DRAM+PMEM nodes
#         - run workload in qemu
#         - run usemem to consume DRAM pages
#         - run sys-refs to migrate hot pages to DRAM, creating LRU pressure so
#           that kernel will migrate cold pages to AEP.  sys-refs will auto
#           exit when all hot pages are roughly in DRAM with this patch.

class VMTest

  attr_accessor :transparent_hugepage
  attr_accessor :qemu_script
  attr_accessor :workload_script
  attr_accessor :vm_workspace

  attr_accessor :dram_nodes
  attr_accessor :pmem_nodes

  attr_accessor :qemu_smp
  attr_accessor :qemu_mem
  attr_accessor :qemu_ssh

  def initialize
    @project_dir = __dir__
    @tests_dir = File.join @project_dir, 'tests'
    @transparent_hugepage = 0
    @qemu_script = "kvm.sh"
    @guest_workspace = "~/test"
    @host_workspace = File.join(@tests_dir, "log")
    @qemu_ssh = "2222"
  end

  def setup_sys
    File.write("/sys/kernel/mm/transparent_hugepage/enabled", @transparent_hugepage)
    File.write("/proc/sys/kernel/numa_balancing", @numa_balancing)
    system("modprobe kvm_ept_idle")
  end

  def spawn_qemu
    env = {
      "interleave" => @all_nodes.join(','),
      "qemu_smp" => @qemu_smp,
      "qemu_mem" => @qemu_mem,
      "qemu_ssh" => @qemu_ssh,
      "qemu_log" => @qemu_log,
    }

    cmd = File.join(@tests_dir, @qemu_script)
    puts "env " + env.map { |k,v| "#{k}=#{v}" }.join(' ') + " " + cmd
    @qemu_pid = Process.spawn(env, cmd)
  end

  def wait_vm
    9.downto(1) do |i|
      sleep(i)
      # mkdir on guest rootfs
      system("ssh", "-p", @qemu_ssh, "root@localhost", "mkdir -p #{@guest_workspace}") && return
    end
    puts "failed to ssh VM"
    Process.kill 'KILL', @qemu_pid
    exit 1
  end

  def stop_qemu
    # record rss in baseline run to guide eat_mem() in next migration run
    read_qemu_rss

    # QEMU may not exit on halt
    system("ssh", "-p", @qemu_ssh, "root@localhost", "/sbin/reboot")
    # sleep 5
    # Process.kill 'KILL', @qemu_pid
    Process.wait @qemu_pid
  end

  def rsync_workload
    cmd = ["rsync", "-a", "-e", "ssh -p #{@qemu_ssh}",
           File.join(@tests_dir, @workload_script), "root@localhost:#{@guest_workspace}/"]
    puts cmd.join(' ')
    system(*cmd)
  end

  def spawn_workload
    cmd = %W[ssh -p #{@qemu_ssh} root@localhost env]
    cmd += @workload_params.map do |k,v| "#{k}=#{v}" end
    cmd << File.join(@guest_workspace, @workload_script)
    puts cmd.join(' ') + " > " + @workload_log
    @workload_pid = Process.spawn(*cmd, [:out, :err]=>[@workload_log, 'w'])
  end

  def wait_workload_startup
    if @workload_script =~ /sysbench/
      wait_log_message(@workload_log, "Threads started")
      # sysbench has allocated all memory at this point, so RSS here can be immediately used by eat_mem()
      read_qemu_rss
    else
      sleep 5
    end
  end

  def wait_log_message(log, msg, seconds = 300)
    seconds.times do
      sleep 1
      return if File.read(log).include? msg
    end
    puts "WARNING: timeout waiting for '#{msg}' in #{log}"
  end

  def read_qemu_rss
    # only necessary if start on pmem nodes, then migrate to dram nodes
    # or if qemu RSS will keep growing after wait_workload_startup()
    return
    proc_status = ProcStatus.new
    proc_status.load(@qemu_pid)
    @qemu_rss_kb = proc_status["VmRSS"].to_i
    puts "QEMU RSS: #{@qemu_rss_kb >> 10}M"
  end

  def eat_mem
    # rss_per_node = @qemu_rss_kb / @dram_nodes.size
    proc_vmstat = ProcVmstat.new
    @usemem_pids = []
    @dram_nodes.each do |nid|
      numa_vmstat = proc_vmstat.numa_vmstat[nid]
      free_kb = numa_vmstat['nr_free_pages'] + numa_vmstat['nr_inactive_file']
      free_kb *= ProcVmstat::PAGE_SIZE >> 10
      puts "Node #{nid}: free #{free_kb >> 10}M"
      spawn_usemem(nid, free_kb)
      # spawn_usemem(nid, free_kb - rss_per_node)
    end
  end

  def spawn_usemem(nid, kb)
    if kb < 0
      puts "WARNING: negative kb = #{kb}"
      return
    end
    cmd = "numactl --membind #{nid} usemem --sleep -1 --step 2m --mlock --prefault #{kb >> 10}m"
    puts cmd
    @usemem_pids << Process.spawn(cmd)
  end

  def spawn_migrate
    cmd = "#{@project_dir}/#{@scheme['migrate_cmd']} -c #{@project_dir}/#{@scheme['migrate_config']}"
    puts cmd + " > " + @migrate_log
    @migrate_pid = Process.spawn(cmd, [:out, :err]=>[@migrate_log, 'w'])
  end

  def run_one(should_migrate = false)
    path_params = @workload_params.map { |k,v| "#{k}=#{v}" }.join('#')
    path_params += '.' + @migrate_script if should_migrate
    log_dir = File.join(@host_workspace, @time_dir, "ratio=#{@ratio}", path_params)
    @workload_log = File.join(log_dir, @workload_script + ".log")
    @migrate_log  = File.join(log_dir, @migrate_script  + ".log")
    @qemu_log     = File.join(log_dir, @qemu_script     + ".log")

    puts '-' * 80
    puts "#{Time.now}  Running test with params #{@workload_params} should_migrate=#{should_migrate}"

    # Avoid this dependency in old RHEL
    #   require "FileUtils"
    #   FileUtils.mkdir_p(log_dir)
    system('mkdir', '-p', log_dir) # on host rootfs

    spawn_qemu
    wait_vm

    rsync_workload
    spawn_workload

    if should_migrate
      wait_workload_startup
      eat_mem
      spawn_migrate
    end

    Process.wait @workload_pid

    if should_migrate
      Process.kill 'KILL', @migrate_pid
      @usemem_pids.each do |pid| Process.kill 'KILL', pid end
    end

    stop_qemu
  end

  def setup_nodes(ratio)
    # this func assumes d <= p
    d = @scheme["dram_nodes"].size
    p = @scheme["pmem_nodes"].size

    # d, p, ratio: 2, 4, 4 => 1, 4, 4
    if d * ratio > p
      d = p / ratio   # pure PMEM if (ratio > p)
    end

    # d, p, ratio: 2, 4, 1 => 2, 2, 1
    if d > 0
      p = d * ratio   # pure DRAM if (ratio == 0)
    end

    # performance can more comparable for
    #   d p ratio
    #   1 1 1
    #   1 2 2
    #   1 4 4
    # than with
    #   2 2 1
    #   2 4 2
    #   1 4 4
    if @scheme["single_dram_node"]
      # d, p, ratio: 2, 2, 1 => 1, 1, 1
      if d > 1
        p /= d
        d = 1
      end
    end

    # dram_nodes/pmem_nodes in scheme are physically available nodes
    # dram_nodes/pmem_nodes in class are to be used in test runs
    @dram_nodes = @scheme["dram_nodes"][0, d]
    @pmem_nodes = @scheme["pmem_nodes"][0, p]
    @all_nodes = @dram_nodes + @pmem_nodes
  end

  def run_group
    @scheme["workload_params"].each do |params|
      @workload_params = params
      run_one
      run_one should_migrate: true unless @dram_nodes.empty?
    end
  end

  def run_all(config_file)
    @scheme = YAML.load_file(config_file)
    @workload_script = @scheme["workload_script"]
    @migrate_script = @scheme["migrate_cmd"].partition(' ')[0]
    @time_dir = Time.now.strftime("%F.%T")
    @scheme["ratios"].each do |ratio|
      @ratio = ratio
      setup_nodes(ratio)
      run_group
    end
  end

end