//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/chimp/chimp_scan.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/chimp/chimp.hpp"

#include "duckdb/common/limits.hpp"
#include "duckdb/common/types/null_value.hpp"
#include "duckdb/function/compression/compression.hpp"
#include "duckdb/function/compression_function.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/storage/statistics/numeric_statistics.hpp"
#include "duckdb/storage/table/column_data_checkpointer.hpp"
#include "duckdb/storage/table/column_segment.hpp"
#include "duckdb/common/operator/subtract.hpp"

#include "flag_buffer.hpp"
#include "leading_zero_buffer.hpp"

namespace duckdb {

struct ChimpGroupState {
public:
	void Load(uint8_t *packed_flags, uint8_t *packed_leading_zeros, const idx_t &group_size,
	          const idx_t &leading_zero_block_size) {
		LoadFlags(packed_flags, group_size);
		LoadLeadingZeros(packed_leading_zeros, leading_zero_block_size);
		index = 0;
		leading_zero_index = 0;
	}
	bool Started() const {
		return !!index;
	}
	void IncreaseLeadingZeroIndex() {
		leading_zero_index++;
	}
	const uint8_t &GetLeadingZero() const {
		//! +1 because we increase the index after every use
		//! We just need to make sure we don't actually need this 1-past-the-end index
		D_ASSERT(leading_zero_index <= max_leading_zeros_to_read + 1);
		return leading_zeros[leading_zero_index];
	}
	const uint8_t &GetFlag() {
		static thread_local uint64_t count = 0;
		count++;
		if (count == 434085) {
			;
		}
		D_ASSERT(index <= max_flags_to_read);
		D_ASSERT(index <= 1024);
		return flags[index++];
	}
	// Assuming the group is completely full
	idx_t RemainingInGroup() const {
		return ChimpPrimitives::CHIMP_SEQUENCE_SIZE - index;
	}

private:
	void LoadFlags(uint8_t *packed_data, idx_t group_size) {
		duckdb_chimp::FlagBuffer<false> flag_buffer;
		flag_buffer.SetBuffer(packed_data);
		flags[0] = 0; // First value doesn't require a flag
		for (idx_t i = 0; i < group_size; i++) {
			flags[1 + i] = flag_buffer.Extract();
		}
		max_flags_to_read = group_size;
	}
	void LoadLeadingZeros(uint8_t *packed_data, idx_t leading_zero_block_size) {
		duckdb_chimp::LeadingZeroBuffer<false> leading_zero_buffer;
		leading_zero_buffer.SetBuffer(packed_data);
		for (idx_t i = 0; i < leading_zero_block_size; i++) {
			leading_zeros[i] = leading_zero_buffer.Extract();
		}
		max_leading_zeros_to_read = leading_zero_block_size;
	}

private:
	idx_t index;
	uint8_t flags[ChimpPrimitives::CHIMP_SEQUENCE_SIZE + 1];
	uint8_t leading_zeros[ChimpPrimitives::CHIMP_SEQUENCE_SIZE + 1];
	uint32_t leading_zero_index;

private:
	idx_t max_flags_to_read;
	idx_t max_leading_zeros_to_read;
};

template <class T>
struct ChimpScanState : public SegmentScanState {
public:
	explicit ChimpScanState(ColumnSegment &segment) : segment(segment) {
		auto &buffer_manager = BufferManager::GetBufferManager(segment.db);

		handle = buffer_manager.Pin(segment.block);
		auto dataptr = handle.Ptr();
		// ScanStates never exceed the boundaries of a Segment,
		// but are not guaranteed to start at the beginning of the Block
		auto start_of_data_segment = dataptr + segment.GetBlockOffset() + ChimpPrimitives::HEADER_SIZE;
		chimp_state.input.SetStream(start_of_data_segment);
		auto metadata_offset = Load<uint32_t>(dataptr + segment.GetBlockOffset());
		metadata_ptr = dataptr + segment.GetBlockOffset() + metadata_offset;
		LoadGroup();
	}

	duckdb_chimp::Chimp128DecompressionState chimp_state;
	BufferHandle handle;
	data_ptr_t metadata_ptr;
	idx_t total_value_count = 0;
	ChimpGroupState group_state;

	ColumnSegment &segment;

	idx_t LeftInGroup() const {
		return ChimpPrimitives::CHIMP_SEQUENCE_SIZE - (total_value_count & (ChimpPrimitives::CHIMP_SEQUENCE_SIZE - 1));
	}

	bool GroupFinished() const {
		return (total_value_count & (ChimpPrimitives::CHIMP_SEQUENCE_SIZE - 1)) == 0;
	}

	// Scan a group from the start
	template <class CHIMP_TYPE>
	void ScanGroup(CHIMP_TYPE *values, idx_t group_size) {
		D_ASSERT(group_size <= ChimpPrimitives::CHIMP_SEQUENCE_SIZE);

		// Increase the internal index used for the flags
		(void)group_state.GetFlag();
		duckdb_chimp::Chimp128Decompression<CHIMP_TYPE>::LoadFirst(*(values), chimp_state);
		for (idx_t i = 1; i < group_size; i++) {
			const bool refresh_leading_zero = duckdb_chimp::Chimp128Decompression<CHIMP_TYPE>::DecompressValue(
			    *(values + i), group_state.GetFlag(), group_state.GetLeadingZero(), chimp_state);
			if (refresh_leading_zero) {
				//! The leading zero value was used, need to get the next one
				group_state.IncreaseLeadingZeroIndex();
			}
		}
		total_value_count += group_size;
		if (GroupFinished() && total_value_count < segment.count) {
			LoadGroup();
		}
	}

	// Scan up to a group boundary
	template <class CHIMP_TYPE>
	void ScanPartialGroup(CHIMP_TYPE *values, idx_t group_size) {
		D_ASSERT(group_size <= ChimpPrimitives::CHIMP_SEQUENCE_SIZE);

		for (idx_t i = 0; i < group_size; i++) {
			const bool refresh_leading_zero = duckdb_chimp::Chimp128Decompression<CHIMP_TYPE>::Load(
			    *(values + i), group_state.GetFlag(), group_state.GetLeadingZero(), chimp_state);
			if (refresh_leading_zero) {
				//! The leading zero value was used, need to get the next one
				group_state.IncreaseLeadingZeroIndex();
			}
		}
		total_value_count += group_size;
		if (GroupFinished() && total_value_count < segment.count) {
			LoadGroup();
		}
	}

	void LoadGroup() {
		chimp_state.Reset();

		// Load the offset indicating where a groups data starts
		metadata_ptr -= sizeof(uint32_t);
		auto data_bit_offset = Load<uint32_t>(metadata_ptr);
		//  Only used for point queries
		(void)data_bit_offset;

		// Load how many blocks of leading zero bits we have
		metadata_ptr -= sizeof(uint8_t);
		auto leading_zero_block_count = Load<uint8_t>(metadata_ptr);

		// Load the leading zero blocks
		metadata_ptr -= 3 * leading_zero_block_count;
		auto leading_zero_blocks = metadata_ptr;

		// Load how many flag bytes there are
		metadata_ptr -= sizeof(uint16_t);
		auto size_of_group = Load<uint16_t>(metadata_ptr);

		// Load the flags
		metadata_ptr -= size_of_group;
		auto flags = metadata_ptr;
		group_state.Load(flags, leading_zero_blocks, (uint32_t)size_of_group * 4,
		                 (uint32_t)leading_zero_block_count * 8);
	}

public:
	//! Skip the next 'skip_count' values, we don't store the values
	// TODO: use the metadata to determine if we can skip a group
	void Skip(ColumnSegment &segment, idx_t skip_count) {
		using INTERNAL_TYPE = typename ChimpType<T>::type;
		INTERNAL_TYPE buffer[ChimpPrimitives::CHIMP_SEQUENCE_SIZE];

		idx_t to_skip = skip_count;
		while (to_skip) {
			auto skip_size = std::min(to_skip, LeftInGroup());
			if (group_state.Started()) {
				ScanGroup(buffer, skip_size);
			} else {
				ScanPartialGroup(buffer, skip_size);
			}
			to_skip -= skip_size;
		}
	}
};

template <class T>
unique_ptr<SegmentScanState> ChimpInitScan(ColumnSegment &segment) {
	auto result = make_unique_base<SegmentScanState, ChimpScanState<T>>(segment);
	return move(result);
}

//===--------------------------------------------------------------------===//
// Scan base data
//===--------------------------------------------------------------------===//
template <class T>
void ChimpScanPartial(ColumnSegment &segment, ColumnScanState &state, idx_t scan_count, Vector &result,
                      idx_t result_offset) {
	using INTERNAL_TYPE = typename ChimpType<T>::type;
	auto &scan_state = (ChimpScanState<T> &)*state.scan_state;

	T *result_data = FlatVector::GetData<T>(result);
	result.SetVectorType(VectorType::FLAT_VECTOR);

	auto current_result_ptr = (INTERNAL_TYPE *)(result_data + result_offset);

	auto scan_size = std::min(scan_count, scan_state.LeftInGroup());

	if (!scan_state.group_state.Started()) {
		scan_state.template ScanGroup<INTERNAL_TYPE>(current_result_ptr, scan_size);
	} else {
		scan_state.template ScanPartialGroup<INTERNAL_TYPE>(current_result_ptr, scan_size);
	}
	scan_count -= scan_size;
	if (!scan_count) {
		//! Already scanned everything
		return;
	}
	// We know for sure that the last group has ended
	D_ASSERT(!scan_state.group_state.Started());
	scan_state.template ScanGroup<INTERNAL_TYPE>(current_result_ptr + scan_size, scan_count);
}

template <class T>
void ChimpSkip(ColumnSegment &segment, ColumnScanState &state, idx_t skip_count) {
	auto &scan_state = (ChimpScanState<T> &)*state.scan_state;
	scan_state.Skip(segment, skip_count);
}

template <class T>
void ChimpScan(ColumnSegment &segment, ColumnScanState &state, idx_t scan_count, Vector &result) {
	ChimpScanPartial<T>(segment, state, scan_count, result, 0);
}

} // namespace duckdb
