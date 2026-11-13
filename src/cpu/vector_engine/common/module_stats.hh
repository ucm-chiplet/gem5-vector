// SPDX-License-Identifier: BSD-3-Clause

#ifndef __CPU_VECTOR_ENGINE_COMMON_MODULE_STATS_HH__
#define __CPU_VECTOR_ENGINE_COMMON_MODULE_STATS_HH__

#include <functional>
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

    /**
     * Hooks for module-specific intervals kept outside this group, such
     * as the sequencer's head waits and multi-command occupancy.
     *
     * before_dump runs before the group is dumped and must add the open
     * part of each interval up to curTick() and restart it there, so a
     * dump counts the time of its own period. after_reset runs after the
     * counters are cleared and must only restart open intervals at
     * curTick(), discarding the time before the reset. Either may be
     * empty. Both only update statistics.
     */
    void setIntervalHooks(std::function<void()> before_dump,
                          std::function<void()> after_reset);

  private:
    uint64_t inFlight = 0;
    Tick lastUpdate = curTick();
    std::function<void()> beforeDump;
    std::function<void()> afterReset;
    void updateTime();
};

} // namespace gem5::vector_engine

#endif // __CPU_VECTOR_ENGINE_COMMON_MODULE_STATS_HH__
