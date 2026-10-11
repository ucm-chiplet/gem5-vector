// SPDX-License-Identifier: BSD-3-Clause

#include "cpu/vector_engine/common/module_stats.hh"

#include "base/logging.hh"

namespace gem5::vector_engine
{

ModuleStats::ModuleStats(statistics::Group *parent, const std::string &name,
                         const std::string &work_unit)
    : statistics::Group(parent, name.c_str()),
      ADD_STAT(accepted, statistics::units::Count::get(),
               ("Accepted " + work_unit + "; excludes retries").c_str()),
      ADD_STAT(
          completed, statistics::units::Count::get(),
          ("Completed " + work_unit + "; includes terminal faults").c_str()),
      ADD_STAT(retries, statistics::units::Count::get(),
               ("Failed admission attempts for " + work_unit).c_str()),
      ADD_STAT(rejected, statistics::units::Count::get(),
               ("Permanently rejected " + work_unit).c_str()),
      ADD_STAT(faults, statistics::units::Count::get(),
               ("Completed " + work_unit + " reporting a fault").c_str()),
      ADD_STAT(
          elements, statistics::units::Count::get(),
          ("Elements in admitted " + work_unit + "; includes faults").c_str()),
      ADD_STAT(bytes, statistics::units::Byte::get(),
               ("Payload bytes in admitted " + work_unit +
                "; excludes retries, includes faults")
                   .c_str()),
      ADD_STAT(outstanding, statistics::units::Count::get(),
               ("Current unfinished " + work_unit).c_str()),
      ADD_STAT(
          peakOutstanding, statistics::units::Count::get(),
          ("Maximum unfinished " + work_unit + " since stats reset").c_str()),
      ADD_STAT(busyTicks, statistics::units::Tick::get(),
               ("Ticks with at least one unfinished " + work_unit).c_str()),
      ADD_STAT(workTicks, statistics::units::Tick::get(),
               ("Sum of in-flight ticks over " + work_unit +
                "; includes unfinished intervals")
                   .c_str())
{}

void
ModuleStats::updateTime()
{
    const Tick elapsed = curTick() - lastUpdate;
    if (inFlight) {
        busyTicks += elapsed;
        workTicks += elapsed * inFlight;
    }
    lastUpdate = curTick();
}

void
ModuleStats::accept(uint64_t element_count, uint64_t byte_count)
{
    updateTime();
    ++accepted;
    elements += element_count;
    bytes += byte_count;
    outstanding = ++inFlight;
    if (inFlight > peakOutstanding.value()) {
        peakOutstanding = inFlight;
    }
}

void
ModuleStats::complete(bool fault)
{
    panic_if(!inFlight, "VPU statistics completion without admitted work");
    updateTime();
    ++completed;
    if (fault) {
        ++faults;
    }
    outstanding = --inFlight;
}

void
ModuleStats::withdraw(uint64_t element_count, uint64_t byte_count,
                      uint64_t previous_peak)
{
    panic_if(!inFlight || lastUpdate != curTick(),
             "VPU statistics withdrawal must follow an immediate Retry");
    --accepted;
    elements -= element_count;
    bytes -= byte_count;
    outstanding = --inFlight;
    peakOutstanding = previous_peak;
    ++retries;
}

void
ModuleStats::preDumpStats()
{
    updateTime();
    statistics::Group::preDumpStats();
}

void
ModuleStats::resetStats()
{
    statistics::Group::resetStats();
    lastUpdate = curTick();
    outstanding = inFlight;
    peakOutstanding = inFlight;
}

} // namespace gem5::vector_engine
