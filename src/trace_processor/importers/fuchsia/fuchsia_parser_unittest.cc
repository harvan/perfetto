/*
 * Copyright (C) 2022 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "src/trace_processor/importers/fuchsia/fuchsia_trace_parser.h"
#include "src/trace_processor/importers/fuchsia/fuchsia_trace_tokenizer.h"

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "perfetto/trace_processor/trace_blob.h"
#include "src/trace_processor/importers/additional_modules.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/args_translation_table.h"
#include "src/trace_processor/importers/common/clock_tracker.h"
#include "src/trace_processor/importers/common/event_tracker.h"
#include "src/trace_processor/importers/common/flow_tracker.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/importers/default_modules.h"
#include "src/trace_processor/importers/ftrace/sched_event_tracker.h"
#include "src/trace_processor/importers/proto/metadata_tracker.h"
#include "src/trace_processor/importers/proto/proto_trace_parser.h"
#include "src/trace_processor/importers/proto/stack_profile_tracker.h"
#include "src/trace_processor/storage/metadata.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/trace_sorter.h"
#include "src/trace_processor/util/descriptors.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/common/builtin_clock.pbzero.h"
#include "protos/perfetto/common/sys_stats_counters.pbzero.h"
#include "protos/perfetto/config/trace_config.pbzero.h"
#include "protos/perfetto/trace/android/packages_list.pbzero.h"
#include "protos/perfetto/trace/chrome/chrome_benchmark_metadata.pbzero.h"
#include "protos/perfetto/trace/chrome/chrome_trace_event.pbzero.h"
#include "protos/perfetto/trace/clock_snapshot.pbzero.h"
#include "protos/perfetto/trace/ftrace/ftrace.pbzero.h"
#include "protos/perfetto/trace/ftrace/ftrace_event.pbzero.h"
#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.pbzero.h"
#include "protos/perfetto/trace/ftrace/generic.pbzero.h"
#include "protos/perfetto/trace/ftrace/power.pbzero.h"
#include "protos/perfetto/trace/ftrace/sched.pbzero.h"
#include "protos/perfetto/trace/ftrace/task.pbzero.h"
#include "protos/perfetto/trace/interned_data/interned_data.pbzero.h"
#include "protos/perfetto/trace/profiling/profile_packet.pbzero.h"
#include "protos/perfetto/trace/ps/process_tree.pbzero.h"
#include "protos/perfetto/trace/sys_stats/sys_stats.pbzero.h"
#include "protos/perfetto/trace/trace.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "protos/perfetto/trace/track_event/chrome_thread_descriptor.pbzero.h"
#include "protos/perfetto/trace/track_event/counter_descriptor.pbzero.h"
#include "protos/perfetto/trace/track_event/debug_annotation.pbzero.h"
#include "protos/perfetto/trace/track_event/log_message.pbzero.h"
#include "protos/perfetto/trace/track_event/process_descriptor.pbzero.h"
#include "protos/perfetto/trace/track_event/source_location.pbzero.h"
#include "protos/perfetto/trace/track_event/task_execution.pbzero.h"
#include "protos/perfetto/trace/track_event/thread_descriptor.pbzero.h"
#include "protos/perfetto/trace/track_event/track_descriptor.pbzero.h"
#include "protos/perfetto/trace/track_event/track_event.pbzero.h"

namespace perfetto {
namespace trace_processor {
namespace {
using ::testing::_;
using ::testing::Args;
using ::testing::AtLeast;
using ::testing::DoAll;
using ::testing::ElementsAreArray;
using ::testing::Eq;
using ::testing::HasSubstr;
using ::testing::IgnoreResult;
using ::testing::InSequence;
using ::testing::Invoke;
using ::testing::InvokeArgument;
using ::testing::NiceMock;
using ::testing::Pointwise;
using ::testing::Return;
using ::testing::ReturnRef;
using ::testing::UnorderedElementsAreArray;
class MockSchedEventTracker : public SchedEventTracker {
 public:
  explicit MockSchedEventTracker(TraceProcessorContext* context)
      : SchedEventTracker(context) {}

  MOCK_METHOD9(PushSchedSwitch,
               void(uint32_t cpu,
                    int64_t timestamp,
                    uint32_t prev_pid,
                    base::StringView prev_comm,
                    int32_t prev_prio,
                    int64_t prev_state,
                    uint32_t next_pid,
                    base::StringView next_comm,
                    int32_t next_prio));
};

class MockProcessTracker : public ProcessTracker {
 public:
  explicit MockProcessTracker(TraceProcessorContext* context)
      : ProcessTracker(context) {}

  MOCK_METHOD4(SetProcessMetadata,
               UniquePid(uint32_t pid,
                         base::Optional<uint32_t> ppid,
                         base::StringView process_name,
                         base::StringView cmdline));

  MOCK_METHOD3(UpdateThreadName,
               UniqueTid(uint32_t tid,
                         StringId thread_name_id,
                         ThreadNamePriority priority));
  MOCK_METHOD3(UpdateThreadNameByUtid,
               void(UniqueTid utid,
                    StringId thread_name_id,
                    ThreadNamePriority priority));
  MOCK_METHOD2(UpdateThread, UniqueTid(uint32_t tid, uint32_t tgid));

  MOCK_METHOD1(GetOrCreateProcess, UniquePid(uint32_t pid));
  MOCK_METHOD2(SetProcessNameIfUnset,
               void(UniquePid upid, StringId process_name_id));
};
class MockBoundInserter : public ArgsTracker::BoundInserter {
 public:
  MockBoundInserter()
      : ArgsTracker::BoundInserter(&tracker_, nullptr, 0u), tracker_(nullptr) {
    ON_CALL(*this, AddArg(_, _, _, _)).WillByDefault(ReturnRef(*this));
  }

  MOCK_METHOD4(
      AddArg,
      ArgsTracker::BoundInserter&(StringId flat_key,
                                  StringId key,
                                  Variadic v,
                                  ArgsTracker::UpdatePolicy update_policy));

 private:
  ArgsTracker tracker_;
};

class MockEventTracker : public EventTracker {
 public:
  explicit MockEventTracker(TraceProcessorContext* context)
      : EventTracker(context) {}
  virtual ~MockEventTracker() = default;

  MOCK_METHOD9(PushSchedSwitch,
               void(uint32_t cpu,
                    int64_t timestamp,
                    uint32_t prev_pid,
                    base::StringView prev_comm,
                    int32_t prev_prio,
                    int64_t prev_state,
                    uint32_t next_pid,
                    base::StringView next_comm,
                    int32_t next_prio));

  MOCK_METHOD3(PushCounter,
               base::Optional<CounterId>(int64_t timestamp,
                                         double value,
                                         TrackId track_id));
};

class MockSliceTracker : public SliceTracker {
 public:
  explicit MockSliceTracker(TraceProcessorContext* context)
      : SliceTracker(context) {}

  MOCK_METHOD5(Begin,
               base::Optional<SliceId>(int64_t timestamp,
                                       TrackId track_id,
                                       StringId cat,
                                       StringId name,
                                       SetArgsCallback args_callback));
  MOCK_METHOD5(End,
               base::Optional<SliceId>(int64_t timestamp,
                                       TrackId track_id,
                                       StringId cat,
                                       StringId name,
                                       SetArgsCallback args_callback));
  MOCK_METHOD6(Scoped,
               base::Optional<SliceId>(int64_t timestamp,
                                       TrackId track_id,
                                       StringId cat,
                                       StringId name,
                                       int64_t duration,
                                       SetArgsCallback args_callback));
  MOCK_METHOD4(StartSlice,
               base::Optional<SliceId>(int64_t timestamp,
                                       TrackId track_id,
                                       SetArgsCallback args_callback,
                                       std::function<SliceId()> inserter));
};

class FuchsiaTraceParserTest : public ::testing::Test {
 public:
  FuchsiaTraceParserTest() {
    context_.storage.reset(new TraceStorage());
    storage_ = context_.storage.get();
    context_.track_tracker.reset(new TrackTracker(&context_));
    context_.global_args_tracker.reset(
        new GlobalArgsTracker(context_.storage.get()));
    context_.global_stack_profile_tracker.reset(
        new GlobalStackProfileTracker());
    context_.args_tracker.reset(new ArgsTracker(&context_));
    context_.args_translation_table.reset(new ArgsTranslationTable(storage_));
    context_.metadata_tracker.reset(
        new MetadataTracker(context_.storage.get()));
    event_ = new MockEventTracker(&context_);
    context_.event_tracker.reset(event_);
    sched_ = new MockSchedEventTracker(&context_);
    context_.sched_tracker.reset(sched_);
    process_ = new NiceMock<MockProcessTracker>(&context_);
    context_.process_tracker.reset(process_);
    slice_ = new NiceMock<MockSliceTracker>(&context_);
    context_.slice_tracker.reset(slice_);
    context_.slice_translation_table.reset(new SliceTranslationTable(storage_));
    context_.clock_tracker.reset(new ClockTracker(context_.storage.get()));
    clock_ = context_.clock_tracker.get();
    context_.flow_tracker.reset(new FlowTracker(&context_));
    context_.sorter.reset(new TraceSorter(&context_, CreateParser(),
                                          TraceSorter::SortingMode::kFullSort));
    context_.descriptor_pool_.reset(new DescriptorPool());

    RegisterDefaultModules(&context_);
    RegisterAdditionalModules(&context_);
  }

  void push_word(uint64_t word) { trace_bytes_.push_back(word); }

  void ResetTraceBuffers() {
    trace_bytes_.clear();
    // Write the FXT Magic Bytes
    push_word(0x0016547846040010);
  }

  void SetUp() override { ResetTraceBuffers(); }

  util::Status Tokenize() {
    const size_t num_bytes = trace_bytes_.size() * sizeof(uint64_t);
    std::unique_ptr<uint8_t[]> raw_trace(new uint8_t[num_bytes]);
    memcpy(raw_trace.get(), trace_bytes_.data(), num_bytes);
    context_.chunk_reader.reset(new FuchsiaTraceTokenizer(&context_));
    auto status = context_.chunk_reader->Parse(TraceBlobView(
        TraceBlob::TakeOwnership(std::move(raw_trace), num_bytes)));

    ResetTraceBuffers();
    return status;
  }

 protected:
  std::vector<uint64_t> trace_bytes_;
  std::unique_ptr<TraceParser> CreateParser() {
    return std::unique_ptr<TraceParser>(new FuchsiaTraceParser(&context_));
  }

  TraceProcessorContext context_;
  MockEventTracker* event_;
  MockSchedEventTracker* sched_;
  MockProcessTracker* process_;
  MockSliceTracker* slice_;
  ClockTracker* clock_;
  TraceStorage* storage_;
};

TEST_F(FuchsiaTraceParserTest, CorruptedFxt) {
  // Invalid record of size 0
  push_word(0x0016547846040000);
  EXPECT_FALSE(Tokenize().ok());
}

TEST_F(FuchsiaTraceParserTest, InlineInstantEvent) {
  // Inline name of 8 bytes
  uint64_t name_ref = uint64_t{0x8008} << 48;
  // Inline category of 8 bytes
  uint64_t category_ref = uint64_t{0x8008} << 32;
  // Inline threadref
  uint64_t threadref = uint64_t{0};
  // Instant Event
  uint64_t event_type = 0 << 16;
  uint64_t size = 6 << 4;
  uint64_t record_type = 4;

  auto header =
      name_ref | category_ref | threadref | event_type | size | record_type;
  push_word(header);
  // Timestamp
  push_word(0xAAAAAAAAAAAAAAAA);
  // Pid + tid
  push_word(0xBBBBBBBBBBBBBBBB);
  push_word(0xCCCCCCCCCCCCCCCC);
  // Inline Category
  push_word(0xDDDDDDDDDDDDDDDD);
  // Inline Name
  push_word(0xEEEEEEEEEEEEEEEE);
  EXPECT_TRUE(Tokenize().ok());
  EXPECT_EQ(context_.storage->stats()[stats::fuchsia_invalid_event].value, 0);
}

TEST_F(FuchsiaTraceParserTest, FxtWithProtos) {
  // Serialize some protos to bytes
  protozero::HeapBuffered<protos::pbzero::Trace> protos;
  {
    auto* packet = protos->add_packet();
    packet->set_trusted_packet_sequence_id(1);
    packet->set_incremental_state_cleared(true);
    auto* thread_desc = packet->set_thread_descriptor();
    thread_desc->set_pid(15);
    thread_desc->set_tid(16);
    thread_desc->set_reference_timestamp_us(1000);
    thread_desc->set_reference_thread_time_us(2000);
  }
  {
    auto* packet = protos->add_packet();
    packet->set_trusted_packet_sequence_id(1);
    auto* event = packet->set_track_event();
    event->set_timestamp_delta_us(10);   // absolute: 1010.
    event->set_thread_time_delta_us(5);  // absolute: 2005.
    event->add_category_iids(1);
    auto* legacy_event = event->set_legacy_event();
    legacy_event->set_name_iid(1);
    legacy_event->set_phase('B');
  }
  {
    auto* packet = protos->add_packet();
    packet->set_trusted_packet_sequence_id(1);
    auto* event = packet->set_track_event();
    event->set_timestamp_delta_us(10);   // absolute: 1020.
    event->set_thread_time_delta_us(5);  // absolute: 2010.
    event->add_category_iids(1);
    auto* legacy_event = event->set_legacy_event();
    legacy_event->set_name_iid(1);
    legacy_event->set_phase('E');
  }
  {
    auto* packet = protos->add_packet();
    packet->set_trusted_packet_sequence_id(1);
    auto* event = packet->set_track_event();
    event->set_timestamp_absolute_us(1005);
    event->set_thread_time_absolute_us(2003);
    event->add_category_iids(2);
    event->add_category_iids(3);
    auto* legacy_event = event->set_legacy_event();
    legacy_event->set_name_iid(2);
    legacy_event->set_phase('X');
    legacy_event->set_duration_us(23);         // absolute end: 1028.
    legacy_event->set_thread_duration_us(12);  // absolute end: 2015.
  }

  protos->Finalize();
  std::vector<uint8_t> perfetto_bytes = protos.SerializeAsArray();

  // Set up an FXT Perfetto Blob Header
  uint64_t blob_type_perfetto = uint64_t{3} << 48;
  uint64_t unpadded_blob_size_bytes = uint64_t{perfetto_bytes.size()} << 32;
  uint64_t blob_name_ref = uint64_t{0x8008} << 16;
  uint64_t size_words = ((perfetto_bytes.size() + 7) / 8 + 2) << 4;
  uint64_t record_type = 5;

  uint64_t header = blob_type_perfetto | unpadded_blob_size_bytes |
                    blob_name_ref | size_words | record_type;

  // Pad the blob to a multiple of 8 bytes.
  while (perfetto_bytes.size() % 8) {
    perfetto_bytes.push_back(0);
  }

  push_word(header);
  // Inline Name Ref
  push_word(0xBBBBBBBBBBBBBBBB);
  trace_bytes_.insert(trace_bytes_.end(),
                      reinterpret_cast<uint64_t*>(perfetto_bytes.data()),
                      reinterpret_cast<uint64_t*>(perfetto_bytes.data() +
                                                  perfetto_bytes.size()));
  EXPECT_CALL(*process_, UpdateThread(16, 15)).WillRepeatedly(Return(1u));

  tables::ThreadTable::Row row(16);
  row.upid = 1u;
  storage_->mutable_thread_table()->Insert(row);

  MockBoundInserter inserter;

  StringId unknown_cat = storage_->InternString("unknown(1)");
  ASSERT_NE(storage_, nullptr);

  constexpr TrackId track{0u};
  constexpr TrackId thread_time_track{1u};

  InSequence in_sequence;  // Below slices should be sorted by timestamp.
  // Only the begin thread time can be imported into the counter table.
  EXPECT_CALL(*event_, PushCounter(1005000, testing::DoubleEq(2003000),
                                   thread_time_track));
  EXPECT_CALL(*slice_, StartSlice(1005000, track, _, _))
      .WillOnce(DoAll(IgnoreResult(InvokeArgument<3>()),
                      InvokeArgument<2>(&inserter), Return(SliceId(0u))));
  EXPECT_CALL(*event_, PushCounter(1010000, testing::DoubleEq(2005000),
                                   thread_time_track));
  EXPECT_CALL(*slice_, StartSlice(1010000, track, _, _))
      .WillOnce(DoAll(IgnoreResult(InvokeArgument<3>()),
                      InvokeArgument<2>(&inserter), Return(SliceId(1u))));
  EXPECT_CALL(*event_, PushCounter(1020000, testing::DoubleEq(2010000),
                                   thread_time_track));
  EXPECT_CALL(*slice_, End(1020000, track, unknown_cat, kNullStringId, _))
      .WillOnce(DoAll(InvokeArgument<4>(&inserter), Return(SliceId(1u))));

  auto status = Tokenize();
  EXPECT_TRUE(status.ok());
  context_.sorter->ExtractEventsForced();
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
