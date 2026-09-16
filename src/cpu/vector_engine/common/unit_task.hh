#ifndef __CPU_VECTOR_ENGINE_COMMON_UNIT_TASK_HH__
#define __CPU_VECTOR_ENGINE_COMMON_UNIT_TASK_HH__

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>

#include "cpu/vector_engine/common/command_key.hh"
#include "cpu/vector_engine/common/vector_types.hh"

namespace gem5::vector_engine
{

using TaskId = uint32_t;

inline constexpr TaskId InvalidTaskId =
    std::numeric_limits<TaskId>::max();

/** Identity of one internal task belonging to a vector command. */
struct TaskKey
{
    CommandKey command;
    TaskId taskId = InvalidTaskId;

    constexpr bool
    valid() const
    {
        return command.valid() && taskId != InvalidTaskId;
    }
};

constexpr bool
operator==(const TaskKey &lhs, const TaskKey &rhs)
{
    return lhs.command == rhs.command && lhs.taskId == rhs.taskId;
}

constexpr bool
operator!=(const TaskKey &lhs, const TaskKey &rhs)
{
    return !(lhs == rhs);
}

constexpr bool
operator<(const TaskKey &lhs, const TaskKey &rhs)
{
    return lhs.command < rhs.command ||
           (lhs.command == rhs.command && lhs.taskId < rhs.taskId);
}

/** Hash function for task-indexed backend state. */
struct TaskKeyHash
{
    std::size_t
    operator()(const TaskKey &task) const
    {
        const auto command_hash = CommandKeyHash{}(task.command);
        const auto task_hash = std::hash<TaskId>{}(task.taskId);

        return command_hash ^
               (task_hash + 0x9e3779b9U +
                (command_hash << 6) + (command_hash >> 2));
    }
};

/** A non-empty half-open interval of logical vector elements. */
struct ElementRange
{
    uint32_t firstElement = 0;
    uint32_t elementCount = 0;

    constexpr bool
    valid() const
    {
        return elementCount != 0 &&
               firstElement <=
                   std::numeric_limits<uint32_t>::max() - elementCount;
    }

    constexpr uint32_t
    end() const
    {
        return firstElement + elementCount;
    }

    constexpr bool
    fitsWithin(uint32_t vstart, uint32_t vl) const
    {
        return valid() && vstart <= vl &&
               firstElement >= vstart && end() <= vl;
    }
};

constexpr bool
operator==(const ElementRange &lhs, const ElementRange &rhs)
{
    return lhs.firstElement == rhs.firstElement &&
           lhs.elementCount == rhs.elementCount;
}

constexpr bool
operator!=(const ElementRange &lhs, const ElementRange &rhs)
{
    return !(lhs == rhs);
}

/** Common envelope propagated with every baseline backend task. */
struct UnitTask
{
    TaskKey key;
    VectorUnitClass unit = VectorUnitClass::Invalid;
    ElementRange elements;

    constexpr bool
    valid() const
    {
        return key.valid() && unit != VectorUnitClass::Invalid &&
               elements.valid();
    }
};

constexpr bool
operator==(const UnitTask &lhs, const UnitTask &rhs)
{
    return lhs.key == rhs.key && lhs.unit == rhs.unit &&
           lhs.elements == rhs.elements;
}

constexpr bool
operator!=(const UnitTask &lhs, const UnitTask &rhs)
{
    return !(lhs == rhs);
}

} // namespace gem5::vector_engine

#endif // __CPU_VECTOR_ENGINE_COMMON_UNIT_TASK_HH__
