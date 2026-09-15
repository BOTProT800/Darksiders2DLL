#include "resource_identity.h"

namespace ds2::modding {

bool IsResourceIdentitySampleUsable(
    const ResourceIdentitySample& sample,
    const std::uintptr_t expected_scope_caller_rva,
    const std::uintptr_t expected_read_caller_rva) noexcept {
    return sample.sequence != 0 && sample.nesting_depth != 0 &&
        sample.read_ordinal != 0 && sample.scope.object_fields_valid &&
        sample.scope.stream_valid && sample.scope.stream != 0 &&
        sample.scope.member_table_offset >= 0 && sample.read.stream != 0 &&
        sample.scope.caller_rva == expected_scope_caller_rva &&
        sample.read.caller_rva == expected_read_caller_rva;
}

ResourceIdentityBeginResult ResourceIdentityThreadContext::Begin(
    const ResourceIdentityScopeInput& input,
    const std::uint64_t sequence) noexcept {
    ResourceIdentityBeginResult result;
    result.token.sequence = sequence;
    result.token.nesting_depth = depth_ + suppressed_depth_ + 1;

    if (suppressed_depth_ != 0 || depth_ == frames_.size()) {
        ++suppressed_depth_;
        result.status = ResourceIdentityBeginStatus::suppressed_overflow;
        result.token.tracked = false;
        return result;
    }

    Frame& frame = frames_[depth_];
    frame.input = input;
    frame.sequence = sequence;
    frame.read_count = 0;
    ++depth_;

    result.status = ResourceIdentityBeginStatus::tracked;
    result.token.tracked = true;
    return result;
}

ResourceIdentityObserveStatus ResourceIdentityThreadContext::ObserveRead(
    const ResourceIdentityReadInput& input,
    ResourceIdentitySample& sample) noexcept {
    sample = {};
    if (suppressed_depth_ != 0) {
        return ResourceIdentityObserveStatus::suppressed_overflow;
    }
    if (depth_ == 0) {
        return ResourceIdentityObserveStatus::no_active_scope;
    }

    Frame& frame = frames_[depth_ - 1];
    if (!frame.input.stream_valid || frame.input.stream == 0) {
        return ResourceIdentityObserveStatus::invalid_scope_stream;
    }
    if (input.stream == 0) {
        return ResourceIdentityObserveStatus::invalid_read_stream;
    }
    if (frame.read_count == kResourceIdentityMaxReadsPerScope) {
        return ResourceIdentityObserveStatus::read_limit_reached;
    }
    ++frame.read_count;

    sample.sequence = frame.sequence;
    sample.nesting_depth = static_cast<std::uint32_t>(depth_);
    sample.read_ordinal = frame.read_count;
    sample.scope = frame.input;
    sample.read = input;
    return ResourceIdentityObserveStatus::recorded;
}

ResourceIdentityEndStatus ResourceIdentityThreadContext::End(
    const ResourceIdentityScopeToken& token) noexcept {
    const std::size_t current_nesting_depth = depth_ + suppressed_depth_;
    if (!token.tracked) {
        if (suppressed_depth_ == 0 ||
            token.nesting_depth != current_nesting_depth) {
            Reset();
            return ResourceIdentityEndStatus::context_reset;
        }
        --suppressed_depth_;
        return ResourceIdentityEndStatus::suppressed_overflow_unwound;
    }

    if (suppressed_depth_ != 0 || depth_ == 0 ||
        token.nesting_depth != depth_ ||
        frames_[depth_ - 1].sequence != token.sequence) {
        Reset();
        return ResourceIdentityEndStatus::context_reset;
    }

    frames_[depth_ - 1] = {};
    --depth_;
    return ResourceIdentityEndStatus::matched;
}

void ResourceIdentityThreadContext::Reset() noexcept {
    frames_ = {};
    depth_ = 0;
    suppressed_depth_ = 0;
}

std::size_t ResourceIdentityThreadContext::ActiveDepth() const noexcept {
    return depth_;
}

std::size_t ResourceIdentityThreadContext::SuppressedDepth() const noexcept {
    return suppressed_depth_;
}

bool ResourceIdentityThreadContext::Empty() const noexcept {
    return depth_ == 0 && suppressed_depth_ == 0;
}

}  // namespace ds2::modding
