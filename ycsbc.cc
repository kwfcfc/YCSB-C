//
//  ycsbc.cc
//  YCSB-C
//
//  Created by Jinglei Ren on 12/19/14.
//  Copyright (c) 2014 Jinglei Ren <jinglei@ren.systems>.
//

#include <cstring>
#include <cstdint>
#include <chrono>
#include <string>
#include <iostream>
#include <vector>
#include <future>
#include <cstdlib>
#include "core/utils.h"
#include "core/timer.h"
#include "core/client.h"
#include "core/core_workload.h"
#include "core/measurements.h"
#include "db/db_factory.h"

using namespace std;

void UsageMessage(const char *command);
bool StrStartWith(const char *str, const char *pre);
string ParseCommandLine(int argc, const char *argv[], utils::Properties &props);

struct ThreadRunResult {
  int oks;
  ycsbc::ThreadMeasurements measurements;

  ThreadRunResult() : oks(0) { }
};

int OpsForThread(int total_ops, int num_threads, int thread_id) {
  return total_ops / num_threads + (thread_id < total_ops % num_threads ? 1 : 0);
}

ThreadRunResult DelegateClient(ycsbc::DB *db, ycsbc::CoreWorkload *wl,
    const int num_ops, bool is_loading, bool measure_latency) {
  db->Init();
  ycsbc::Client client(*db, *wl);
  ThreadRunResult result;
  for (int i = 0; i < num_ops; ++i) {
    if (is_loading) {
      result.oks += client.DoInsert();
    } else {
      ycsbc::Operation operation = ycsbc::READ;
      uint64_t latency_us = 0;
      bool ok = client.DoTransaction(&operation,
          measure_latency ? &latency_us : NULL);
      result.oks += ok;
      if (measure_latency) {
        result.measurements.Record(operation, latency_us, ok);

        // Trigger Magictrace dump on tail latency spike (>1ms)
        if (latency_us > 1000) {
          std::cerr << "\n[!] Anomaly detected: " << latency_us
                    << " us. Freezing Magictrace buffer." << std::endl;
          int ret = system("pkill -INT magictrace");
          (void)ret; // Suppress unused result warning

          break; // Stop execution to preserve the hardware trace
        }
      }
    }
  }
  db->Close();
  return result;
}

int main(const int argc, const char *argv[]) {
  utils::Properties props;
  string file_name = ParseCommandLine(argc, argv, props);

  ycsbc::DB *db = ycsbc::DBFactory::CreateDB(props);
  if (!db) {
    cout << "Unknown database name " << props["dbname"] << endl;
    exit(0);
  }

  ycsbc::CoreWorkload wl;
  wl.Init(props);

  const int num_threads = stoi(props.GetProperty("threadcount", "1"));
  const bool quiet = stoi(props.GetProperty("quiet", "0")) != 0;
  const bool latency = stoi(props.GetProperty("latency", "0")) != 0;
  const vector<double> percentiles = ycsbc::ParsePercentiles(
      props.GetProperty("latencypercentiles", "50,95,99,99.9"));

  // Loads data
  vector<future<ThreadRunResult>> actual_ops;
  int total_ops = stoi(props[ycsbc::CoreWorkload::RECORD_COUNT_PROPERTY]);
  for (int i = 0; i < num_threads; ++i) {
    actual_ops.emplace_back(async(launch::async,
        DelegateClient, db, &wl, OpsForThread(total_ops, num_threads, i), true,
        false));
  }
  assert((int)actual_ops.size() == num_threads);

  int sum = 0;
  for (auto &n : actual_ops) {
    assert(n.valid());
    sum += n.get().oks;
  }
  if (not quiet) {
    cerr << "# Loading records:\t" << sum << endl;
  }

  // Peforms transactions
  actual_ops.clear();
  total_ops = stoi(props[ycsbc::CoreWorkload::OPERATION_COUNT_PROPERTY]);
  utils::Timer<double> timer;
  ycsbc::Measurements measurements;
  timer.Start();
  for (int i = 0; i < num_threads; ++i) {
    actual_ops.emplace_back(async(launch::async,
        DelegateClient, db, &wl, OpsForThread(total_ops, num_threads, i),
        false, latency));
  }
  assert((int)actual_ops.size() == num_threads);

  sum = 0;
  for (auto &n : actual_ops) {
    assert(n.valid());
    ThreadRunResult result = n.get();
    sum += result.oks;
    if (latency) {
      measurements.Merge(&result.measurements);
    }
  }
  double duration = timer.End();
  if (latency) {
    if (not quiet) {
      cerr << "# Latency summary (us)" << endl;
      cerr << "# Benchmark\t" << props["dbname"] << '\t' << file_name << '\t'
           << num_threads << endl;
    }
    measurements.PrintSummary(cerr, percentiles);
  } else {
    if (not quiet) {
      cerr << "# Transaction throughput (KTPS)" << endl;
      cerr << props["dbname"] << '\t' << file_name << '\t' << num_threads
           << '\t';
    }
    cerr << total_ops / duration / 1000 << endl;
  }
}

string ParseCommandLine(int argc, const char *argv[], utils::Properties &props) {
  int argindex = 1;
  string filename;
  while (argindex < argc && StrStartWith(argv[argindex], "-")) {
    if (strcmp(argv[argindex], "-threads") == 0) {
      argindex++;
      if (argindex >= argc) {
        UsageMessage(argv[0]);
        exit(0);
      }
      props.SetProperty("threadcount", argv[argindex]);
      argindex++;
    } else if (strcmp(argv[argindex], "-db") == 0) {
      argindex++;
      if (argindex >= argc) {
        UsageMessage(argv[0]);
        exit(0);
      }
      props.SetProperty("dbname", argv[argindex]);
      argindex++;
    } else if (strcmp(argv[argindex], "-host") == 0) {
      argindex++;
      if (argindex >= argc) {
        UsageMessage(argv[0]);
        exit(0);
      }
      props.SetProperty("host", argv[argindex]);
      argindex++;
    } else if (strcmp(argv[argindex], "-port") == 0) {
      argindex++;
      if (argindex >= argc) {
        UsageMessage(argv[0]);
        exit(0);
      }
      props.SetProperty("port", argv[argindex]);
      argindex++;
    } else if (strcmp(argv[argindex], "-slaves") == 0) {
      argindex++;
      if (argindex >= argc) {
        UsageMessage(argv[0]);
        exit(0);
      }
      props.SetProperty("slaves", argv[argindex]);
      argindex++;
    } else if (strcmp(argv[argindex], "-P") == 0) {
      argindex++;
      if (argindex >= argc) {
        UsageMessage(argv[0]);
        exit(0);
      }
      filename.assign(argv[argindex]);
      ifstream input(argv[argindex]);
      try {
        props.Load(input);
      } catch (const string &message) {
        cout << message << endl;
        exit(0);
      }
      input.close();
      argindex++;
    } else if (strcmp(argv[argindex], "-quiet") == 0) {
      props.SetProperty("quiet", "1");
      argindex++;
    } else if (strcmp(argv[argindex], "-latency") == 0) {
      props.SetProperty("latency", "1");
      argindex++;
    } else {
      cout << "Unknown option '" << argv[argindex] << "'" << endl;
      exit(0);
    }
  }

  if (argindex == 1 || argindex != argc) {
    UsageMessage(argv[0]);
    exit(0);
  }

  return filename;
}

void UsageMessage(const char *command) {
  cout << "Usage: " << command << " [options]" << endl;
  cout << "Options:" << endl;
  cout << "  -threads n: execute using n threads (default: 1)" << endl;
  cout << "  -db dbname: specify the name of the DB to use (default: basic)" << endl;
  cout << "  -quiet: only print benchmark result" << endl;
  cout << "  -latency: print latency percentiles (in microseconds) instead of throughput" << endl;
  cout << "  -P propertyfile: load properties from the given file. Multiple files can" << endl;
  cout << "                   be specified, and will be processed in the order specified" << endl;
}

inline bool StrStartWith(const char *str, const char *pre) {
  return strncmp(str, pre, strlen(pre)) == 0;
}