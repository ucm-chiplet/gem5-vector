// SPDX-License-Identifier: BSD-3-Clause

#ifndef __CPU_VECTOR_ENGINE_COMMON_MODULE_STATS_HH__
#define __CPU_VECTOR_ENGINE_COMMON_MODULE_STATS_HH__

#include <string>

#include "base/statistics.hh"
#include "sim/cur_tick.hh"

namespace gem5::vector_engine
{

/**
 * Statistics only: never schedule events or change admission decisions.
 * The module defines its work item (command, task, fragment or access).
 * Count admission once, completion before the external callback, and Retry
 * per failed attempt. Bytes/elements describe admitted work, including work
 * which may later fault. Occupancy includes unfinished work across resets.
 * Time accounting includes partial intervals at dump/reset without polling.
 */
struct ModuleStats : public statistics::Group
{
    ModuleStats(statistics::Group *parent, const std::string &name,
                const std::string &work_unit);

    statistics::Scalar accepted;
    statistics::Scalar completed;
    statistics::Scalar retries;
    statistics::Scalar rejected;
    statistics::Scalar faults;
    statistics::Scalar elements;
    statistics::Scalar bytes;
    statistics::Scalar outstanding;
    statistics::Scalar peakOutstanding;
    statistics::Scalar busyTicks;
    statistics::Scalar workTicks;

    void accept(uint64_t element_count = 0, uint64_t byte_count = 0);
    void complete(bool fault = false);
    // Undo a provisional admission when a synchronous sender returns Retry.
    void withdraw(uint64_t element_count, uint64_t byte_count,
                  uint64_t previous_peak);
    void preDumpStats() override;
    void resetStats() override;

  private:
    uint64_t inFlight = 0;
    Tick lastUpdate = curTick();
    void updateTime();
};

} // namespace gem5::vector_engine

#endif // __CPU_VECTOR_ENGINE_COMMON_MODULE_STATS_HH__
