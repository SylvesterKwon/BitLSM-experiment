#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <string>

class ResourceMonitor {
 public:
  ResourceMonitor(const std::string& sample_path,
                  const std::string& thread_path)
      : sample_csv_(sample_path), thread_csv_(thread_path),
        start_(std::chrono::steady_clock::now()) {
    sample_csv_ << "timestamp_ns,insertions,rss_kb\n";
    thread_csv_
        << "timestamp_ns,insertions,tid,comm,utime_ticks,stime_ticks\n";
  }

  void Sample(uint64_t insertions) {
    auto ts_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                     std::chrono::steady_clock::now() - start_)
                     .count();

    long rss_kb = ReadRssKb();
    sample_csv_ << ts_ns << "," << insertions << "," << rss_kb << "\n";
    sample_csv_.flush();

    DIR* dir = opendir("/proc/self/task");
    if (!dir) return;
    while (auto* ent = readdir(dir)) {
      const char* tid = ent->d_name;
      if (tid[0] == '.') continue;

      std::string comm = ReadComm(tid);
      long utime = 0, stime = 0;
      ReadCpuTicks(tid, utime, stime);

      thread_csv_ << ts_ns << "," << insertions << "," << tid << "," << comm
                  << "," << utime << "," << stime << "\n";
    }
    closedir(dir);
    thread_csv_.flush();
  }

 private:
  static long ReadRssKb() {
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line)) {
      if (line.compare(0, 6, "VmRSS:") == 0) {
        long kb = 0;
        std::sscanf(line.c_str() + 6, "%ld", &kb);
        return kb;
      }
    }
    return 0;
  }

  static std::string ReadComm(const char* tid) {
    std::string path = std::string("/proc/self/task/") + tid + "/comm";
    std::ifstream f(path);
    std::string comm;
    std::getline(f, comm);
    while (!comm.empty() && (comm.back() == '\n' || comm.back() == '\r')) {
      comm.pop_back();
    }
    for (auto& c : comm) {
      if (c == ',') c = '_';
    }
    return comm;
  }

  static void ReadCpuTicks(const char* tid, long& utime, long& stime) {
    std::string path = std::string("/proc/self/task/") + tid + "/stat";
    std::ifstream f(path);
    std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
    auto rparen = content.rfind(')');
    if (rparen == std::string::npos) return;
    // Fields after "comm" — stat fields 3..N. utime is field 14, stime is 15.
    // After ')' there is " <state> <ppid> ..." — token index 0 is state.
    // utime → token index 11, stime → token index 12.
    std::string rest = content.substr(rparen + 1);
    int idx = 0;
    size_t pos = 0;
    while (pos < rest.size()) {
      while (pos < rest.size() && rest[pos] == ' ') pos++;
      if (pos >= rest.size()) break;
      size_t end = rest.find(' ', pos);
      if (end == std::string::npos) end = rest.size();
      if (idx == 11) utime = std::strtol(rest.c_str() + pos, nullptr, 10);
      else if (idx == 12) {
        stime = std::strtol(rest.c_str() + pos, nullptr, 10);
        return;
      }
      idx++;
      pos = end;
    }
  }

  std::ofstream sample_csv_;
  std::ofstream thread_csv_;
  std::chrono::steady_clock::time_point start_;
};
