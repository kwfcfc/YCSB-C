#ifndef YCSB_C_MEASUREMENTS_H_
#define YCSB_C_MEASUREMENTS_H_

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iterator>
#include <limits>
#include <numeric>
#include <ostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "core_workload.h"

namespace ycsbc {

inline size_t OperationIndex(Operation op) {
  switch (op) {
    case READ:
      return 0;
    case UPDATE:
      return 1;
    case INSERT:
      return 2;
    case SCAN:
      return 3;
    case READMODIFYWRITE:
      return 4;
    default:
      throw utils::Exception("Operation request is not recognized!");
  }
}

inline const char *OperationName(Operation op) {
  switch (op) {
    case READ:
      return "READ";
    case UPDATE:
      return "UPDATE";
    case INSERT:
      return "INSERT";
    case SCAN:
      return "SCAN";
    case READMODIFYWRITE:
      return "READMODIFYWRITE";
    default:
      return "UNKNOWN";
  }
}

inline const std::array<Operation, 5> &AllOperations() {
  static const std::array<Operation, 5> operations = {
      {READ, UPDATE, INSERT, SCAN, READMODIFYWRITE}};
  return operations;
}

inline std::string FormatPercentileLabel(double percentile) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(3) << percentile;
  std::string label = out.str();
  while (!label.empty() && label[label.size() - 1] == '0') {
    label.erase(label.size() - 1);
  }
  if (!label.empty() && label[label.size() - 1] == '.') {
    label.erase(label.size() - 1);
  }
  return std::string("p").append(label);
}

struct OperationMeasurements {
  std::vector<uint64_t> latencies_us;
  size_t ok_count;

  OperationMeasurements() : ok_count(0) { }

  void Record(uint64_t latency_us, bool ok) {
    latencies_us.push_back(latency_us);
    if (ok) {
      ++ok_count;
    }
  }

  size_t Count() const {
    return latencies_us.size();
  }
};

struct ThreadMeasurements {
  std::array<OperationMeasurements, 5> operations;

  void Record(Operation op, uint64_t latency_us, bool ok) {
    operations[OperationIndex(op)].Record(latency_us, ok);
  }
};

struct LatencySummary {
  size_t count;
  size_t ok_count;
  uint64_t min_us;
  uint64_t max_us;
  double avg_us;
  std::vector<std::pair<double, uint64_t>> percentiles;

  LatencySummary()
      : count(0), ok_count(0), min_us(0), max_us(0), avg_us(0.0) { }
};

inline LatencySummary BuildLatencySummary(std::vector<uint64_t> latencies_us,
    size_t ok_count, const std::vector<double> &percentiles) {
  LatencySummary summary;
  summary.count = latencies_us.size();
  summary.ok_count = ok_count;
  if (latencies_us.empty()) {
    return summary;
  }

  std::sort(latencies_us.begin(), latencies_us.end());
  summary.min_us = latencies_us.front();
  summary.max_us = latencies_us.back();
  summary.avg_us = static_cast<double>(
      std::accumulate(latencies_us.begin(), latencies_us.end(), 0.0L) /
      latencies_us.size());

  for (double percentile : percentiles) {
    long double rank = percentile / 100.0L * (latencies_us.size() - 1);
    size_t index = static_cast<size_t>(std::ceil(rank));
    if (index >= latencies_us.size()) {
      index = latencies_us.size() - 1;
    }
    summary.percentiles.push_back(
        std::make_pair(percentile, latencies_us[index]));
  }
  return summary;
}

class Measurements {
 public:
  void Merge(ThreadMeasurements *thread_measurements) {
    for (size_t i = 0; i < AllOperations().size(); ++i) {
      OperationMeasurements &source = thread_measurements->operations[i];
      OperationMeasurements &target = operations_[i];
      target.ok_count += source.ok_count;
      target.latencies_us.insert(target.latencies_us.end(),
          std::make_move_iterator(source.latencies_us.begin()),
          std::make_move_iterator(source.latencies_us.end()));
      source.latencies_us.clear();
      source.ok_count = 0;
    }
  }

  bool Empty() const {
    for (const auto &op : operations_) {
      if (!op.latencies_us.empty()) {
        return false;
      }
    }
    return true;
  }

  void PrintSummary(std::ostream &out,
      const std::vector<double> &percentiles) const {
    out << "op\tcount\tok\tmin_us\tmax_us\tavg_us";
    for (double percentile : percentiles) {
      out << '\t' << FormatPercentileLabel(percentile);
    }
    out << '\n';

    std::vector<uint64_t> overall_latencies;
    size_t overall_ok = 0;
    size_t overall_count = 0;
    for (const auto &op : operations_) {
      overall_count += op.latencies_us.size();
    }
    overall_latencies.reserve(overall_count);
    for (const auto &op : operations_) {
      overall_ok += op.ok_count;
      overall_latencies.insert(overall_latencies.end(),
          op.latencies_us.begin(), op.latencies_us.end());
    }
    PrintSummaryLine(out, "OVERALL",
        BuildLatencySummary(overall_latencies, overall_ok, percentiles));

    for (Operation op : AllOperations()) {
      const OperationMeasurements &data = operations_[OperationIndex(op)];
      if (data.Count() == 0) {
        continue;
      }
      PrintSummaryLine(out, OperationName(op),
          BuildLatencySummary(data.latencies_us, data.ok_count, percentiles));
    }
  }

 private:
  static void PrintSummaryLine(std::ostream &out, const std::string &name,
      const LatencySummary &summary) {
    out << name << '\t'
        << summary.count << '\t'
        << summary.ok_count << '\t'
        << summary.min_us << '\t'
        << summary.max_us << '\t'
        << std::fixed << std::setprecision(2) << summary.avg_us;
    for (const auto &value : summary.percentiles) {
      out << '\t' << value.second;
    }
    out << '\n';
  }

  std::array<OperationMeasurements, 5> operations_;
};

inline std::vector<double> ParsePercentiles(const std::string &spec) {
  std::vector<double> percentiles;
  std::stringstream input(spec);
  std::string token;
  while (std::getline(input, token, ',')) {
    if (token.empty()) {
      continue;
    }
    double percentile = std::stod(token);
    if (percentile <= 0.0 || percentile >= 100.0) {
      throw utils::Exception("Latency percentile must be between 0 and 100: " +
          token);
    }
    percentiles.push_back(percentile);
  }

  if (percentiles.empty()) {
    throw utils::Exception("No valid latency percentiles were provided.");
  }

  std::sort(percentiles.begin(), percentiles.end());
  return percentiles;
}

} // ycsbc

#endif // YCSB_C_MEASUREMENTS_H_
