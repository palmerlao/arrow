// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include <gtest/gtest.h>

#include "arrow/testing/gtest_compat.h"

#include "parquet/column_reader.h"
#include "parquet/column_writer.h"
#include "parquet/file_reader.h"
#include "parquet/file_writer.h"
#include "parquet/platform.h"
#include "parquet/test_util.h"
#include "parquet/types.h"

namespace parquet {

using schema::GroupNode;
using schema::NodePtr;
using schema::PrimitiveNode;

namespace test {

template <typename TestType>
class TestSerialize : public PrimitiveTypedTest<TestType> {
 public:
  typedef typename TestType::c_type T;

  void SetUp() {
    num_columns_ = 4;
    num_rowgroups_ = 4;
    rows_per_rowgroup_ = 50;
    rows_per_batch_ = 10;
    this->SetUpSchema(Repetition::OPTIONAL, num_columns_);
  }

 protected:
  int num_columns_;
  int num_rowgroups_;
  int rows_per_rowgroup_;
  int rows_per_batch_;

  void FileSerializeTest(Compression::type codec_type) {
    auto sink = CreateOutputStream();
    auto gnode = std::static_pointer_cast<GroupNode>(this->node_);

    WriterProperties::Builder prop_builder;

    for (int i = 0; i < num_columns_; ++i) {
      prop_builder.compression(this->schema_.Column(i)->name(), codec_type);
    }
    std::shared_ptr<WriterProperties> writer_properties = prop_builder.build();

    auto file_writer = ParquetFileWriter::Open(sink, gnode, writer_properties);
    this->GenerateData(rows_per_rowgroup_);
    for (int rg = 0; rg < num_rowgroups_ / 2; ++rg) {
      RowGroupWriter* row_group_writer;
      row_group_writer = file_writer->AppendRowGroup();
      for (int col = 0; col < num_columns_; ++col) {
        auto column_writer =
            static_cast<TypedColumnWriter<TestType>*>(row_group_writer->NextColumn());
        column_writer->WriteBatch(rows_per_rowgroup_, this->def_levels_.data(), nullptr,
                                  this->values_ptr_);
        column_writer->Close();
        // Ensure column() API which is specific to BufferedRowGroup cannot be called
        ASSERT_THROW(row_group_writer->column(col), ParquetException);
      }

      row_group_writer->Close();
    }
    // Write half BufferedRowGroups
    for (int rg = 0; rg < num_rowgroups_ / 2; ++rg) {
      RowGroupWriter* row_group_writer;
      row_group_writer = file_writer->AppendBufferedRowGroup();
      for (int batch = 0; batch < (rows_per_rowgroup_ / rows_per_batch_); ++batch) {
        for (int col = 0; col < num_columns_; ++col) {
          auto column_writer =
              static_cast<TypedColumnWriter<TestType>*>(row_group_writer->column(col));
          column_writer->WriteBatch(
              rows_per_batch_, this->def_levels_.data() + (batch * rows_per_batch_),
              nullptr, this->values_ptr_ + (batch * rows_per_batch_));
          // Ensure NextColumn() API which is specific to RowGroup cannot be called
          ASSERT_THROW(row_group_writer->NextColumn(), ParquetException);
        }
      }
      for (int col = 0; col < num_columns_; ++col) {
        auto column_writer =
            static_cast<TypedColumnWriter<TestType>*>(row_group_writer->column(col));
        column_writer->Close();
      }
      row_group_writer->Close();
    }
    file_writer->Close();

    PARQUET_ASSIGN_OR_THROW(auto buffer, sink->Finish());

    int num_rows_ = num_rowgroups_ * rows_per_rowgroup_;

    auto source = std::make_shared<::arrow::io::BufferReader>(buffer);
    auto file_reader = ParquetFileReader::Open(source);
    ASSERT_EQ(num_columns_, file_reader->metadata()->num_columns());
    ASSERT_EQ(num_rowgroups_, file_reader->metadata()->num_row_groups());
    ASSERT_EQ(num_rows_, file_reader->metadata()->num_rows());

    for (int rg = 0; rg < num_rowgroups_; ++rg) {
      auto rg_reader = file_reader->RowGroup(rg);
      ASSERT_EQ(num_columns_, rg_reader->metadata()->num_columns());
      ASSERT_EQ(rows_per_rowgroup_, rg_reader->metadata()->num_rows());
      // Check that the specified compression was actually used.
      ASSERT_EQ(codec_type, rg_reader->metadata()->ColumnChunk(0)->compression());

      int64_t values_read;

      for (int i = 0; i < num_columns_; ++i) {
        ASSERT_FALSE(rg_reader->metadata()->ColumnChunk(i)->has_index_page());
        std::vector<int16_t> def_levels_out(rows_per_rowgroup_);
        std::vector<int16_t> rep_levels_out(rows_per_rowgroup_);
        auto col_reader =
            std::static_pointer_cast<TypedColumnReader<TestType>>(rg_reader->Column(i));
        this->SetupValuesOut(rows_per_rowgroup_);
        col_reader->ReadBatch(rows_per_rowgroup_, def_levels_out.data(),
                              rep_levels_out.data(), this->values_out_ptr_, &values_read);
        this->SyncValuesOut();
        ASSERT_EQ(rows_per_rowgroup_, values_read);
        ASSERT_EQ(this->values_, this->values_out_);
        ASSERT_EQ(this->def_levels_, def_levels_out);
      }
    }
  }

  void UnequalNumRows(int64_t max_rows, const std::vector<int64_t> rows_per_column) {
    auto sink = CreateOutputStream();
    auto gnode = std::static_pointer_cast<GroupNode>(this->node_);

    std::shared_ptr<WriterProperties> props = WriterProperties::Builder().build();

    auto file_writer = ParquetFileWriter::Open(sink, gnode, props);

    RowGroupWriter* row_group_writer;
    row_group_writer = file_writer->AppendRowGroup();

    this->GenerateData(max_rows);
    for (int col = 0; col < num_columns_; ++col) {
      auto column_writer =
          static_cast<TypedColumnWriter<TestType>*>(row_group_writer->NextColumn());
      column_writer->WriteBatch(rows_per_column[col], this->def_levels_.data(), nullptr,
                                this->values_ptr_);
      column_writer->Close();
    }
    row_group_writer->Close();
    file_writer->Close();
  }

  void UnequalNumRowsBuffered(int64_t max_rows,
                              const std::vector<int64_t> rows_per_column) {
    auto sink = CreateOutputStream();
    auto gnode = std::static_pointer_cast<GroupNode>(this->node_);

    std::shared_ptr<WriterProperties> props = WriterProperties::Builder().build();

    auto file_writer = ParquetFileWriter::Open(sink, gnode, props);

    RowGroupWriter* row_group_writer;
    row_group_writer = file_writer->AppendBufferedRowGroup();

    this->GenerateData(max_rows);
    for (int col = 0; col < num_columns_; ++col) {
      auto column_writer =
          static_cast<TypedColumnWriter<TestType>*>(row_group_writer->column(col));
      column_writer->WriteBatch(rows_per_column[col], this->def_levels_.data(), nullptr,
                                this->values_ptr_);
      column_writer->Close();
    }
    row_group_writer->Close();
    file_writer->Close();
  }

  void RepeatedUnequalRows() {
    // Optional and repeated, so definition and repetition levels
    this->SetUpSchema(Repetition::REPEATED);

    const int kNumRows = 100;
    this->GenerateData(kNumRows);

    auto sink = CreateOutputStream();
    auto gnode = std::static_pointer_cast<GroupNode>(this->node_);
    std::shared_ptr<WriterProperties> props = WriterProperties::Builder().build();
    auto file_writer = ParquetFileWriter::Open(sink, gnode, props);

    RowGroupWriter* row_group_writer;
    row_group_writer = file_writer->AppendRowGroup();

    this->GenerateData(kNumRows);

    std::vector<int16_t> definition_levels(kNumRows, 1);
    std::vector<int16_t> repetition_levels(kNumRows, 0);

    {
      auto column_writer =
          static_cast<TypedColumnWriter<TestType>*>(row_group_writer->NextColumn());
      column_writer->WriteBatch(kNumRows, definition_levels.data(),
                                repetition_levels.data(), this->values_ptr_);
      column_writer->Close();
    }

    definition_levels[1] = 0;
    repetition_levels[3] = 1;

    {
      auto column_writer =
          static_cast<TypedColumnWriter<TestType>*>(row_group_writer->NextColumn());
      column_writer->WriteBatch(kNumRows, definition_levels.data(),
                                repetition_levels.data(), this->values_ptr_);
      column_writer->Close();
    }
  }

  void ZeroRowsRowGroup() {
    auto sink = CreateOutputStream();
    auto gnode = std::static_pointer_cast<GroupNode>(this->node_);

    std::shared_ptr<WriterProperties> props = WriterProperties::Builder().build();

    auto file_writer = ParquetFileWriter::Open(sink, gnode, props);

    RowGroupWriter* row_group_writer;

    row_group_writer = file_writer->AppendRowGroup();
    for (int col = 0; col < num_columns_; ++col) {
      auto column_writer =
          static_cast<TypedColumnWriter<TestType>*>(row_group_writer->NextColumn());
      column_writer->Close();
    }
    row_group_writer->Close();

    row_group_writer = file_writer->AppendBufferedRowGroup();
    for (int col = 0; col < num_columns_; ++col) {
      auto column_writer =
          static_cast<TypedColumnWriter<TestType>*>(row_group_writer->column(col));
      column_writer->Close();
    }
    row_group_writer->Close();

    file_writer->Close();
  }
};

typedef ::testing::Types<Int32Type, Int64Type, Int96Type, FloatType, DoubleType,
                         BooleanType, ByteArrayType, FLBAType>
    TestTypes;

TYPED_TEST_SUITE(TestSerialize, TestTypes);

TYPED_TEST(TestSerialize, SmallFileUncompressed) {
  ASSERT_NO_FATAL_FAILURE(this->FileSerializeTest(Compression::UNCOMPRESSED));
}

TYPED_TEST(TestSerialize, TooFewRows) {
  std::vector<int64_t> num_rows = {100, 100, 100, 99};
  ASSERT_THROW(this->UnequalNumRows(100, num_rows), ParquetException);
  ASSERT_THROW(this->UnequalNumRowsBuffered(100, num_rows), ParquetException);
}

TYPED_TEST(TestSerialize, TooManyRows) {
  std::vector<int64_t> num_rows = {100, 100, 100, 101};
  ASSERT_THROW(this->UnequalNumRows(101, num_rows), ParquetException);
  ASSERT_THROW(this->UnequalNumRowsBuffered(101, num_rows), ParquetException);
}

TYPED_TEST(TestSerialize, ZeroRows) { ASSERT_NO_THROW(this->ZeroRowsRowGroup()); }

TYPED_TEST(TestSerialize, RepeatedTooFewRows) {
  ASSERT_THROW(this->RepeatedUnequalRows(), ParquetException);
}

#ifdef ARROW_WITH_SNAPPY
TYPED_TEST(TestSerialize, SmallFileSnappy) {
  ASSERT_NO_FATAL_FAILURE(this->FileSerializeTest(Compression::SNAPPY));
}
#endif

#ifdef ARROW_WITH_BROTLI
TYPED_TEST(TestSerialize, SmallFileBrotli) {
  ASSERT_NO_FATAL_FAILURE(this->FileSerializeTest(Compression::BROTLI));
}
#endif

#ifdef ARROW_WITH_GZIP
TYPED_TEST(TestSerialize, SmallFileGzip) {
  ASSERT_NO_FATAL_FAILURE(this->FileSerializeTest(Compression::GZIP));
}
#endif

#ifdef ARROW_WITH_LZ4
TYPED_TEST(TestSerialize, SmallFileLz4) {
  ASSERT_NO_FATAL_FAILURE(this->FileSerializeTest(Compression::LZ4));
}
#endif

#ifdef ARROW_WITH_ZSTD
TYPED_TEST(TestSerialize, SmallFileZstd) {
  ASSERT_NO_FATAL_FAILURE(this->FileSerializeTest(Compression::ZSTD));
}
#endif

TEST(TestBufferedRowGroupWriter, DisabledDictionary) {
  // PARQUET-1706:
  // Wrong dictionary_page_offset when writing only data pages via BufferedPageWriter
  auto sink = CreateOutputStream();
  auto writer_props = parquet::WriterProperties::Builder().disable_dictionary()->build();
  schema::NodeVector fields;
  fields.push_back(
      PrimitiveNode::Make("col", parquet::Repetition::REQUIRED, parquet::Type::INT32));
  auto schema = std::static_pointer_cast<GroupNode>(
      GroupNode::Make("schema", Repetition::REQUIRED, fields));
  auto file_writer = parquet::ParquetFileWriter::Open(sink, schema, writer_props);
  auto rg_writer = file_writer->AppendBufferedRowGroup();
  auto col_writer = static_cast<Int32Writer*>(rg_writer->column(0));
  int value = 0;
  col_writer->WriteBatch(1, nullptr, nullptr, &value);
  rg_writer->Close();
  file_writer->Close();
  PARQUET_ASSIGN_OR_THROW(auto buffer, sink->Finish());

  auto source = std::make_shared<::arrow::io::BufferReader>(buffer);
  auto file_reader = ParquetFileReader::Open(source);
  ASSERT_EQ(1, file_reader->metadata()->num_row_groups());
  auto rg_reader = file_reader->RowGroup(0);
  ASSERT_EQ(1, rg_reader->metadata()->num_columns());
  ASSERT_EQ(1, rg_reader->metadata()->num_rows());
  ASSERT_FALSE(rg_reader->metadata()->ColumnChunk(0)->has_dictionary_page());
}

TEST(TestBufferedRowGroupWriter, MultiPageDisabledDictionary) {
  const int VALUE_COUNT = 10000;
  const int PAGE_SIZE = 16384;
  auto sink = CreateOutputStream();
  auto writer_props = parquet::WriterProperties::Builder()
                          .disable_dictionary()
                          ->data_pagesize(PAGE_SIZE)
                          ->build();
  schema::NodeVector fields;
  fields.push_back(
      PrimitiveNode::Make("col", parquet::Repetition::REQUIRED, parquet::Type::INT32));
  auto schema = std::static_pointer_cast<GroupNode>(
      GroupNode::Make("schema", Repetition::REQUIRED, fields));
  auto file_writer = parquet::ParquetFileWriter::Open(sink, schema, writer_props);
  auto rg_writer = file_writer->AppendBufferedRowGroup();
  auto col_writer = static_cast<Int32Writer*>(rg_writer->column(0));
  std::vector<int32_t> values_in;
  for (int i = 0; i < VALUE_COUNT; ++i) {
    values_in.push_back((i % 100) + 1);
  }
  col_writer->WriteBatch(VALUE_COUNT, nullptr, nullptr, values_in.data());
  rg_writer->Close();
  file_writer->Close();
  PARQUET_ASSIGN_OR_THROW(auto buffer, sink->Finish());

  auto source = std::make_shared<::arrow::io::BufferReader>(buffer);
  auto file_reader = ParquetFileReader::Open(source);
  auto file_metadata = file_reader->metadata();
  ASSERT_EQ(1, file_reader->metadata()->num_row_groups());
  std::vector<int32_t> values_out(VALUE_COUNT);
  for (int r = 0; r < file_metadata->num_row_groups(); ++r) {
    auto rg_reader = file_reader->RowGroup(r);
    ASSERT_EQ(1, rg_reader->metadata()->num_columns());
    ASSERT_EQ(VALUE_COUNT, rg_reader->metadata()->num_rows());
    int64_t total_values_read = 0;
    std::shared_ptr<parquet::ColumnReader> col_reader;
    ASSERT_NO_THROW(col_reader = rg_reader->Column(0));
    parquet::Int32Reader* int32_reader =
        static_cast<parquet::Int32Reader*>(col_reader.get());
    int64_t vn = VALUE_COUNT;
    int32_t* vx = values_out.data();
    while (int32_reader->HasNext()) {
      int64_t values_read;
      int32_reader->ReadBatch(vn, nullptr, nullptr, vx, &values_read);
      vn -= values_read;
      vx += values_read;
      total_values_read += values_read;
    }
    ASSERT_EQ(VALUE_COUNT, total_values_read);
    ASSERT_EQ(values_in, values_out);
  }
}

}  // namespace test

}  // namespace parquet
