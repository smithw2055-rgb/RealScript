#pragma once

#include "realscript/semantic/Semantic.h"

#include <cstddef>
#include <cstdint>

namespace realscript::runtime {

class AsyncRuntimeContext;
struct CoroutineFrame;

/**
 * Terminal / active lifecycle states for pooled `AwaitableCore` state machines.
 */
enum class AwaitStatus : std::uint8_t {
    Pending = 0,
    Succeeded = 1,
    Faulted = 2,
    Canceled = 3,
};

/**
 * Deterministic main-thread frame phase queues on which continuations resume.
 */
enum class ResumeQueue : std::uint8_t {
    PreTickIo = 0,
    FixedTick = 1,
    Update = 2,
    EndOfFrame = 3,
};

inline constexpr std::size_t kResumeQueueCount = 4;

[[nodiscard]] const char* awaitStatusName(AwaitStatus status) noexcept;
[[nodiscard]] const char* resumeQueueName(ResumeQueue queue) noexcept;
[[nodiscard]] semantic::SymbolId whenAnyResultTypeId() noexcept;
[[nodiscard]] semantic::SymbolId awaitableVoidTypeId() noexcept;
[[nodiscard]] semantic::SymbolId asyncExceptionTypeId() noexcept;
[[nodiscard]] semantic::SymbolId asyncCanceledExceptionTypeId() noexcept;

/**
 * 8-Byte Generational Handle to a pooled `AwaitableCore` slot.
 * Zero `ManagedHeap` allocation; prevents ABA slot-reuse bugs via monotonic 32-bit version.
 */
struct AwaitableHandle {
    static constexpr std::uint32_t InvalidSlot = 0xFFFFFFFFu;
    static constexpr std::uint32_t CompletedVoidSlot = 0xFFFFFFFEu;

    std::uint32_t slot = InvalidSlot;
    std::uint32_t version = 0;

    [[nodiscard]] static constexpr AwaitableHandle invalid() noexcept {
        return AwaitableHandle{InvalidSlot, 0u};
    }

    [[nodiscard]] static constexpr AwaitableHandle completedVoid() noexcept {
        return AwaitableHandle{CompletedVoidSlot, 1u};
    }

    [[nodiscard]] constexpr bool valid() const noexcept {
        return slot != InvalidSlot && version != 0u;
    }

    [[nodiscard]] constexpr bool isCompletedVoidSentinel() const noexcept {
        return slot == CompletedVoidSlot && version == 1u;
    }

    [[nodiscard]] constexpr std::uint64_t pack64() const noexcept {
        return (static_cast<std::uint64_t>(version) << 32u) |
            static_cast<std::uint64_t>(slot);
    }

    [[nodiscard]] static constexpr AwaitableHandle unpack64(std::uint64_t packed) noexcept {
        return AwaitableHandle{
            static_cast<std::uint32_t>(packed & 0xFFFFFFFFu),
            static_cast<std::uint32_t>(packed >> 32u),
        };
    }

    constexpr bool operator==(const AwaitableHandle& other) const noexcept {
        return slot == other.slot && version == other.version;
    }

    constexpr bool operator!=(const AwaitableHandle& other) const noexcept {
        return !(*this == other);
    }
};

static_assert(sizeof(AwaitableHandle) == 8, "AwaitableHandle must remain an 8-byte value handle");

/**
 * 8-Byte Generational Controller Handle to a pooled `CancellationTokenSource` slot.
 */
struct CancellationSourceHandle {
    static constexpr std::uint32_t InvalidSlot = 0xFFFFFFFFu;

    std::uint32_t slot = InvalidSlot;
    std::uint32_t version = 0;

    [[nodiscard]] constexpr bool valid() const noexcept {
        return slot != InvalidSlot && version != 0u;
    }

    [[nodiscard]] constexpr std::uint64_t pack64() const noexcept {
        return (static_cast<std::uint64_t>(version) << 32u) |
            static_cast<std::uint64_t>(slot);
    }

    [[nodiscard]] static constexpr CancellationSourceHandle unpack64(std::uint64_t packed) noexcept {
        return CancellationSourceHandle{
            static_cast<std::uint32_t>(packed & 0xFFFFFFFFu),
            static_cast<std::uint32_t>(packed >> 32u),
        };
    }

    constexpr bool operator==(const CancellationSourceHandle& other) const noexcept {
        return slot == other.slot && version == other.version;
    }

    constexpr bool operator!=(const CancellationSourceHandle& other) const noexcept {
        return !(*this == other);
    }
};

static_assert(sizeof(CancellationSourceHandle) == 8, "CancellationSourceHandle must be 8 bytes");

/**
 * 8-Byte Read-Only Generational `CancellationToken` passed into async operations & coroutines.
 */
struct CancellationToken {
    static constexpr std::uint32_t InvalidSlot = 0xFFFFFFFFu;
    static constexpr std::uint32_t PreCanceledSlot = 0xFFFFFFFEu;

    std::uint32_t ctsSlot = InvalidSlot;
    std::uint32_t version = 0;

    [[nodiscard]] static constexpr CancellationToken none() noexcept {
        return CancellationToken{InvalidSlot, 0u};
    }

    [[nodiscard]] static constexpr CancellationToken preCanceled() noexcept {
        return CancellationToken{PreCanceledSlot, 1u};
    }

    [[nodiscard]] constexpr bool canBeCanceled() const noexcept {
        return ctsSlot != InvalidSlot && version != 0u;
    }

    [[nodiscard]] constexpr bool isPreCanceledSentinel() const noexcept {
        return ctsSlot == PreCanceledSlot && version == 1u;
    }

    [[nodiscard]] constexpr std::uint64_t pack64() const noexcept {
        return (static_cast<std::uint64_t>(version) << 32u) |
            static_cast<std::uint64_t>(ctsSlot);
    }

    [[nodiscard]] static constexpr CancellationToken unpack64(std::uint64_t packed) noexcept {
        return CancellationToken{
            static_cast<std::uint32_t>(packed & 0xFFFFFFFFu),
            static_cast<std::uint32_t>(packed >> 32u),
        };
    }

    constexpr bool operator==(const CancellationToken& other) const noexcept {
        return ctsSlot == other.ctsSlot && version == other.version;
    }

    constexpr bool operator!=(const CancellationToken& other) const noexcept {
        return !(*this == other);
    }
};

static_assert(sizeof(CancellationToken) == 8, "CancellationToken must remain an 8-byte value token");

struct CoroutineFrameHandle {
    std::uint32_t slot = 0;
    std::uint32_t version = 0;

    static constexpr CoroutineFrameHandle invalid() noexcept { return {}; }

    [[nodiscard]] constexpr bool valid() const noexcept { return version != 0; }

    [[nodiscard]] constexpr std::uint64_t pack64() const noexcept {
        return (static_cast<std::uint64_t>(version) << 32u) |
            static_cast<std::uint64_t>(slot);
    }

    [[nodiscard]] static constexpr CoroutineFrameHandle unpack64(
        std::uint64_t packed) noexcept {
        CoroutineFrameHandle h{};
        h.slot = static_cast<std::uint32_t>(packed & 0xFFFFFFFFULL);
        h.version = static_cast<std::uint32_t>(packed >> 32u);
        return h;
    }

    friend constexpr bool operator==(
        CoroutineFrameHandle a,
        CoroutineFrameHandle b) noexcept {
        return a.slot == b.slot && a.version == b.version;
    }
    friend constexpr bool operator!=(
        CoroutineFrameHandle a,
        CoroutineFrameHandle b) noexcept {
        return !(a == b);
    }
};

static_assert(sizeof(CoroutineFrameHandle) == 8, "CoroutineFrameHandle must be 8 bytes");

} // namespace realscript::runtime
