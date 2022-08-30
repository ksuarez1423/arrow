// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements. See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership. The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License. You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied. See the License for the
// specific language governing permissions and limitations
// under the License.

#include <arrow/api.h>
#include <arrow/dataset/api.h>
#include <parquet/arrow/reader.h>
#include <parquet/arrow/writer.h>

#include <iostream>

// Generate some data for the rest of this example.
arrow::Result<std::shared_ptr<arrow::Table>> CreateTable() {
  // This code should look familiar from the basic Arrow example, and is not the
  // focus of this example. However, we need data to work on it, and this makes that!
  auto schema =
      arrow::schema({arrow::field("a", arrow::int64()), arrow::field("b", arrow::int64()),
                     arrow::field("c", arrow::int64())});
  std::shared_ptr<arrow::Array> array_a;
  std::shared_ptr<arrow::Array> array_b;
  std::shared_ptr<arrow::Array> array_c;
  arrow::NumericBuilder<arrow::Int64Type> builder;
  ARROW_RETURN_NOT_OK(builder.AppendValues({0, 1, 2, 3, 4, 5, 6, 7, 8, 9}));
  ARROW_RETURN_NOT_OK(builder.Finish(&array_a));
  builder.Reset();
  ARROW_RETURN_NOT_OK(builder.AppendValues({9, 8, 7, 6, 5, 4, 3, 2, 1, 0}));
  ARROW_RETURN_NOT_OK(builder.Finish(&array_b));
  builder.Reset();
  ARROW_RETURN_NOT_OK(builder.AppendValues({1, 2, 1, 2, 1, 2, 1, 2, 1, 2}));
  ARROW_RETURN_NOT_OK(builder.Finish(&array_c));
  return arrow::Table::Make(schema, {array_a, array_b, array_c});
}

// Set up a dataset by writing two Parquet files.
arrow::Result<std::string> CreateExampleParquetDataset(
    const std::shared_ptr<arrow::fs::FileSystem>& filesystem,
    const std::string& root_path) {
  // Much like CreateTable(), this is utility that gets us the dataset we'll be reading
  // from. Don't worry, we also write a dataset in the example proper.
  auto base_path = root_path + "parquet_dataset";
  ARROW_RETURN_NOT_OK(filesystem->CreateDir(base_path));
  // Create an Arrow Table
  ARROW_ASSIGN_OR_RAISE(auto table, CreateTable());
  // Write it into two Parquet files
  ARROW_ASSIGN_OR_RAISE(auto output,
                        filesystem->OpenOutputStream(base_path + "/data1.parquet"));
  ARROW_RETURN_NOT_OK(parquet::arrow::WriteTable(
      *table->Slice(0, 5), arrow::default_memory_pool(), output, 2048));
  ARROW_ASSIGN_OR_RAISE(output,
                        filesystem->OpenOutputStream(base_path + "/data2.parquet"));
  ARROW_RETURN_NOT_OK(parquet::arrow::WriteTable(
      *table->Slice(5), arrow::default_memory_pool(), output, 2048));
  return base_path;
}

arrow::Status PrepareEnv() {
  // Get our environment prepared for reading, by setting up some quick writing.
  ARROW_ASSIGN_OR_RAISE(auto src_table, CreateTable())
  std::shared_ptr<arrow::fs::FileSystem> setup_fs;
  // Note this operates in the directory the executable is built in.
  char setup_path[256];
  getcwd(setup_path, 256);
  ARROW_ASSIGN_OR_RAISE(setup_fs, arrow::fs::FileSystemFromUriOrPath(setup_path));
  ARROW_ASSIGN_OR_RAISE(auto dset_path, CreateExampleParquetDataset(setup_fs, ""));

  return arrow::Status::OK();
}

arrow::Status RunMain() {
  ARROW_RETURN_NOT_OK(PrepareEnv());

  // First, we need a filesystem object, which lets us interact with our local
  // filesystem starting at a given path. For the sake of simplicity, that'll be
  // the current directory.
  std::shared_ptr<arrow::fs::FileSystem> fs;
  // Get the CWD, use it to make the FileSystem object. 
  char init_path[256];
  getcwd(init_path, 256);
  ARROW_ASSIGN_OR_RAISE(fs, arrow::fs::FileSystemFromUriOrPath(init_path));

  // A file selector lets us actually traverse a multi-file dataset.
  arrow::fs::FileSelector selector;
  selector.base_dir = "parquet_dataset";
  // Recursive is a safe bet if you don't know the nesting of your dataset.
  selector.recursive = true;
  // Making an options object lets us configure our dataset reading.
  arrow::dataset::FileSystemFactoryOptions options;
  // We'll use Hive-style partitioning. We'll let Arrow Datasets infer the partition
  // schema. We won't set any other options, defaults are fine.
  options.partitioning = arrow::dataset::HivePartitioning::MakeFactory();
  auto read_format = std::make_shared<arrow::dataset::ParquetFileFormat>();
  // Now, we get a factory that will let us get our dataset -- we don't have the
  // dataset yet!
  ARROW_ASSIGN_OR_RAISE(auto factory, arrow::dataset::FileSystemDatasetFactory::Make(
                                          fs, selector, read_format, options));
  // Now we build our dataset from the factory.
  ARROW_ASSIGN_OR_RAISE(auto read_dataset, factory->Finish());
  // Print out the fragments
  ARROW_ASSIGN_OR_RAISE(auto fragments, read_dataset->GetFragments());
  for (const auto& fragment : fragments) {
    std::cout << "Found fragment: " << (*fragment)->ToString() << std::endl;
    std::cout << "Partition expression: "
              << (*fragment)->partition_expression().ToString() << std::endl;
  }

  // Scan dataset into a Table -- once this is done, you can do
  // normal table things with it, like computation and printing. However, now you're
  // also dedicated to being in memory.
  ARROW_ASSIGN_OR_RAISE(auto read_scan_builder, read_dataset->NewScan());
  ARROW_ASSIGN_OR_RAISE(auto read_scanner, read_scan_builder->Finish());
  ARROW_ASSIGN_OR_RAISE(std::shared_ptr<arrow::Table> table, read_scanner->ToTable());
  std::cout << table->ToString();

  // Now, let's get a table out to disk as a dataset!
  // We make a RecordBatchReader from our Table, then set up a scanner, which lets us
  // go to a file.
  std::shared_ptr<arrow::TableBatchReader> write_dataset =
      std::make_shared<arrow::TableBatchReader>(table);
  auto write_scanner_builder =
      arrow::dataset::ScannerBuilder::FromRecordBatchReader(write_dataset);
  ARROW_ASSIGN_OR_RAISE(auto write_scanner, write_scanner_builder->Finish())

  // The partition schema determines which fields are used as keys for partitioning.
  auto partition_schema = arrow::schema({arrow::field("a", arrow::utf8())});
  // We'll use Hive-style partitioning, which creates directories with "key=value"
  // pairs.
  auto partitioning =
      std::make_shared<arrow::dataset::HivePartitioning>(partition_schema);
  // Now, we declare we'll be writing Parquet files.
  auto write_format = std::make_shared<arrow::dataset::ParquetFileFormat>();
  // This time, we make Options for writing, but do much more configuration.
  arrow::dataset::FileSystemDatasetWriteOptions write_options;
  // Defaults to start.
  write_options.file_write_options = write_format->DefaultWriteOptions();
  // Use the filesystem we already have.
  write_options.filesystem = fs;
  // Write to the folder "write_dataset" in current directory.
  write_options.base_dir = "write_dataset";
  // Use the partitioning declared above.
  write_options.partitioning = partitioning;
  // Define what the name for the files making up the dataset will be.
  write_options.basename_template = "part{i}.parquet";
  // Set behavior to overwrite existing data -- specifically, this lets this example
  // be run more than once, and allows whatever code you have to overwrite what's there.
  write_options.existing_data_behavior =
      arrow::dataset::ExistingDataBehavior::kOverwriteOrIgnore;
  // Write to disk!
  ARROW_RETURN_NOT_OK(
      arrow::dataset::FileSystemDataset::Write(write_options, write_scanner));

  return arrow::Status::OK();
}

int main() {
  arrow::Status st = RunMain();
  if (!st.ok()) {
    std::cerr << st << std::endl;
    return 1;
  }
  return 0;
}
